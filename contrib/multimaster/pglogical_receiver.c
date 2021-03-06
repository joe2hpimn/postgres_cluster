/*-------------------------------------------------------------------------
 *
 * receiver_raw.c
 *		Receive and apply logical changes generated by decoder_raw. This
 *		creates some basics for a multi-master cluster using vanilla
 *		PostgreSQL without modifying its code.
 *
 * Copyright (c) 1996-2015, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		receiver_raw/receiver_raw.c
 *
 *-------------------------------------------------------------------------
 */

/* Some general headers for custom bgworker facility */
#include <unistd.h>
#include "postgres.h"
#include "fmgr.h"
#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "access/xact.h"
#include "access/transam.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "executor/spi.h"

#include "multimaster.h"

/* Allow load of this module in shared libs */

typedef struct ReceiverArgs { 
    char* receiver_conn_string;
    char receiver_slot[16];
} ReceiverArgs;

#define ERRCODE_DUPLICATE_OBJECT_STR  "42710"

/* Signal handling */
static volatile sig_atomic_t got_sigterm = false;
static volatile sig_atomic_t got_sighup = false;

/* GUC variables */
static int receiver_idle_time = 0;
static bool receiver_sync_mode = false;

/* Worker name */
static char *worker_name = "multimaster";
char worker_proc[16];

/* Lastly written positions */
static XLogRecPtr output_written_lsn = InvalidXLogRecPtr;
static XLogRecPtr output_fsync_lsn = InvalidXLogRecPtr;
static XLogRecPtr output_applied_lsn = InvalidXLogRecPtr;

/* Stream functions */
static void fe_sendint64(int64 i, char *buf);
static int64 fe_recvint64(char *buf);

static void
receiver_raw_sigterm(SIGNAL_ARGS)
{
	int save_errno = errno;
	got_sigterm = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);
	errno = save_errno;
}

static void
receiver_raw_sighup(SIGNAL_ARGS)
{
	int save_errno = errno;
	got_sighup = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);
	errno = save_errno;
}

/*
 * Send a Standby Status Update message to server.
 */
static bool
sendFeedback(PGconn *conn, int64 now)
{
	char		replybuf[1 + 8 + 8 + 8 + 8 + 1];
	int		 len = 0;

	replybuf[len] = 'r';
	len += 1;
	fe_sendint64(output_written_lsn, &replybuf[len]);   /* write */
	len += 8;
	fe_sendint64(output_fsync_lsn, &replybuf[len]);	 /* flush */
	len += 8;
	fe_sendint64(output_applied_lsn, &replybuf[len]);	/* apply */
	len += 8;
	fe_sendint64(now, &replybuf[len]);  /* sendTime */
	len += 8;

	/* No reply requested from server */
	replybuf[len] = 0;
	len += 1;

	if (PQputCopyData(conn, replybuf, len) <= 0 || PQflush(conn))
	{
		ereport(LOG, (errmsg("%s: could not send feedback packet: %s",
							 worker_proc, PQerrorMessage(conn))));
		return false;
	}

	return true;
}

/*
 * Converts an int64 to network byte order.
 */
static void
fe_sendint64(int64 i, char *buf)
{
	uint32	  n32;

	/* High order half first, since we're doing MSB-first */
	n32 = (uint32) (i >> 32);
	n32 = htonl(n32);
	memcpy(&buf[0], &n32, 4);

	/* Now the low order half */
	n32 = (uint32) i;
	n32 = htonl(n32);
	memcpy(&buf[4], &n32, 4);
}

/*
 * Converts an int64 from network byte order to native format.
 */
static int64
fe_recvint64(char *buf)
{
	int64	   result;
	uint32	  h32;
	uint32	  l32;

	memcpy(&h32, buf, 4);
	memcpy(&l32, buf + 4, 4);
	h32 = ntohl(h32);
	l32 = ntohl(l32);

	result = h32;
	result <<= 32;
	result |= l32;

	return result;
}

static int64
feGetCurrentTimestamp(void)
{
	int64	   result;
	struct timeval tp;

	gettimeofday(&tp, NULL);

	result = (int64) tp.tv_sec -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

	result = (result * USECS_PER_SEC) + tp.tv_usec;

	return result;
}

static void
feTimestampDifference(int64 start_time, int64 stop_time,
					  long *secs, int *microsecs)
{
	int64	   diff = stop_time - start_time;

	if (diff <= 0)
	{
		*secs = 0;
		*microsecs = 0;
	}
	else
	{
		*secs = (long) (diff / USECS_PER_SEC);
		*microsecs = (int) (diff % USECS_PER_SEC);
	}
}

static void
pglogical_receiver_main(Datum main_arg)
{
    ReceiverArgs* args = (ReceiverArgs*)main_arg;
	/* Variables for replication connection */
	PQExpBuffer query;
	PGconn *conn;
	PGresult *res;
    bool insideTrans = false;
    ByteBuffer buf;

	/* Register functions for SIGTERM/SIGHUP management */
	pqsignal(SIGHUP, receiver_raw_sighup);
	pqsignal(SIGTERM, receiver_raw_sigterm);

    sprintf(worker_proc, "mm_recv_%d", getpid());

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to a database */
	BackgroundWorkerInitializeConnection(MMDatabaseName, NULL);

	/* Establish connection to remote server */
	conn = PQconnectdb(args->receiver_conn_string);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		PQfinish(conn);
		ereport(ERROR, (errmsg("%s: Could not establish connection to remote server",
							 worker_proc)));
		proc_exit(1);
	}

	query = createPQExpBuffer();

    appendPQExpBuffer(query, "CREATE_REPLICATION_SLOT \"%s\" LOGICAL \"%s\"", args->receiver_slot, worker_name);
    res = PQexec(conn, query->data);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
 		const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
        if (!sqlstate || strcmp(sqlstate, ERRCODE_DUPLICATE_OBJECT_STR) != 0)
		{
            PQclear(res);
            ereport(ERROR, (errmsg("%s: Could not create logical slot",
                                 worker_proc)));
            proc_exit(1);
        }
    }
    PQclear(res);
	resetPQExpBuffer(query);

	/* Start logical replication at specified position */
	appendPQExpBuffer(query, "START_REPLICATION SLOT \"%s\" LOGICAL 0/0 (\"startup_params_format\" '1', \"max_proto_version\" '1',  \"min_proto_version\" '1')",
					  args->receiver_slot);
	res = PQexec(conn, query->data);
	if (PQresultStatus(res) != PGRES_COPY_BOTH)
	{
		PQclear(res);
		ereport(LOG, (errmsg("%s: Could not start logical replication",
							 worker_proc)));
		proc_exit(1);
	}
	PQclear(res);
	resetPQExpBuffer(query);

    MMReceiverStarted();
    ByteBufferAlloc(&buf);

	while (!got_sigterm)
	{
		int rc, hdr_len;
		/* Buffer for COPY data */
		char	*copybuf = NULL;
		/* Wait necessary amount of time */
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   receiver_idle_time * 1L);
		ResetLatch(&MyProc->procLatch);
		/* Process signals */
		if (got_sighup)
		{
			/* Process config file */
			ProcessConfigFile(PGC_SIGHUP);
			got_sighup = false;
			ereport(LOG, (errmsg("%s: processed SIGHUP", worker_proc)));
		}

		if (got_sigterm)
		{
			/* Simply exit */
			ereport(LOG, (errmsg("%s: processed SIGTERM", worker_proc)));
			proc_exit(0);
		}

		/* Emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		/* Some cleanup */
		if (copybuf != NULL)
		{
			PQfreemem(copybuf);
			copybuf = NULL;
		}

		/*
		 * Receive data.
		 */
		while (true)
		{
			XLogRecPtr  walEnd;
            char* stmt;

			rc = PQgetCopyData(conn, &copybuf, 1);
			if (rc <= 0) {
				break;
            }

			/*
			 * Check message received from server:
			 * - 'k', keepalive message
			 * - 'w', check for streaming header
			 */
			if (copybuf[0] == 'k')
			{
				int			pos;
				bool		replyRequested;

				/*
				 * Parse the keepalive message, enclosed in the CopyData message.
				 * We just check if the server requested a reply, and ignore the
				 * rest.
				 */
				pos = 1;	/* skip msgtype 'k' */

				/*
				 * In this message is the latest WAL position that server has
				 * considered as sent to this receiver.
				 */
				walEnd = fe_recvint64(&copybuf[pos]);
				pos += 8;	/* read walEnd */
				pos += 8;	/* skip sendTime */
				if (rc < pos + 1)
				{
					ereport(LOG, (errmsg("%s: streaming header too small: %d",
										 worker_proc, rc)));
					proc_exit(1);
				}
				replyRequested = copybuf[pos];

				/* Update written position */
				output_written_lsn = Max(walEnd, output_written_lsn);
				output_fsync_lsn = output_written_lsn;
				output_applied_lsn = output_written_lsn;

				/*
				 * If the server requested an immediate reply, send one.
				 * If sync mode is sent reply in all cases to ensure that
				 * server knows how far replay has been done.
				 */
				if (replyRequested || receiver_sync_mode)
				{
					int64 now = feGetCurrentTimestamp();

					/* Leave is feedback is not sent properly */
					if (!sendFeedback(conn, now))
						proc_exit(1);
				}
				continue;
			}
			else if (copybuf[0] != 'w')
			{
				ereport(LOG, (errmsg("%s: Incorrect streaming header",
									 worker_proc)));
				proc_exit(1);
			}

			/* Now fetch the data */
			hdr_len = 1;		/* msgtype 'w' */
			fe_recvint64(&copybuf[hdr_len]);
			hdr_len += 8;		/* dataStart */
			walEnd = fe_recvint64(&copybuf[hdr_len]);
			hdr_len += 8;		/* WALEnd */
			hdr_len += 8;		/* sendTime */

			/*ereport(LOG, (errmsg("%s: receive message %c length %d", worker_proc, copybuf[hdr_len], rc - hdr_len)));*/

            Assert(rc >= hdr_len);

			if (rc > hdr_len)
			{
                stmt = copybuf + hdr_len;
           
#ifdef USE_PGLOGICAL_OUTPUT
                ByteBufferAppend(&buf, stmt, rc - hdr_len);
                if (stmt[0] == 'C') /* commit */
                { 
                    MMExecute(buf.data, buf.used);
                    ByteBufferReset(&buf);
                }
#else
                if (strncmp(stmt, "BEGIN ", 6) == 0) { 
                    TransactionId xid;
                    int rc = sscanf(stmt + 6, "%u", &xid);
                    Assert(rc == 1);
                    ByteBufferAppendInt32(&buf, xid);
                    Assert(!insideTrans);
                    insideTrans = true;
                } else if (strncmp(stmt, "COMMIT;", 7) == 0) { 
                    Assert(insideTrans);
                    Assert(buf.used > 4);
                    buf.data[buf.used-1] = '\0'; /* replace last ';' with '\0' to make string zero terminated */
                    MMExecute(buf.data, buf.used);
                    ByteBufferReset(&buf);
                    insideTrans = false;
                } else {
                    Assert(insideTrans);
                    ByteBufferAppend(&buf, stmt, rc - hdr_len/*strlen(stmt)*/);
                }
#endif
            }
			/* Update written position */
			output_written_lsn = Max(walEnd, output_written_lsn);
			output_fsync_lsn = output_written_lsn;
			output_applied_lsn = output_written_lsn;
		}

		/* No data, move to next loop */
		if (rc == 0)
		{
			/*
			 * In async mode, and no data available. We block on reading but
			 * not more than the specified timeout, so that we can send a
			 * response back to the client.
			 */
			int			r;
			fd_set	  input_mask;
			int64	   message_target = 0;
			int64	   fsync_target = 0;
			struct timeval timeout;
			struct timeval *timeoutptr = NULL;
			int64	   targettime;
			long		secs;
			int		 usecs;
			int64		now;

			FD_ZERO(&input_mask);
			FD_SET(PQsocket(conn), &input_mask);

			/* Now compute when to wakeup. */
			targettime = message_target;

			if (fsync_target > 0 && fsync_target < targettime)
				targettime = fsync_target;
			now = feGetCurrentTimestamp();
			feTimestampDifference(now,
								  targettime,
								  &secs,
								  &usecs);
			if (secs <= 0)
				timeout.tv_sec = 1; /* Always sleep at least 1 sec */
			else
				timeout.tv_sec = secs;
			timeout.tv_usec = usecs;
			timeoutptr = &timeout;

			r = select(PQsocket(conn) + 1, &input_mask, NULL, NULL, timeoutptr);
			if (r == 0 || (r < 0 && errno == EINTR))
			{
				/*
				 * Got a timeout or signal. Continue the loop and either
				 * deliver a status packet to the server or just go back into
				 * blocking.
				 */
				continue;
			}
			else if (r < 0)
			{
				ereport(LOG, (errmsg("%s: Incorrect status received... Leaving.",
									 worker_proc)));
				proc_exit(1);
			}

			/* Else there is actually data on the socket */
			if (PQconsumeInput(conn) == 0)
			{
				ereport(LOG, (errmsg("%s: Data remaining on the socket... Leaving.",
									 worker_proc)));
				proc_exit(1);
			}
			continue;
		}

		/* End of copy stream */
		if (rc == -1)
		{
			ereport(LOG, (errmsg("%s: COPY Stream has abruptly ended...",
								 worker_proc)));
			break;
		}

		/* Failure when reading copy stream, leave */
		if (rc == -2)
		{
			ereport(LOG, (errmsg("%s: Failure while receiving changes...",
								 worker_proc)));
			proc_exit(1);
		}
	}

    ByteBufferFree(&buf);
	/* No problems, so clean exit */
	proc_exit(0);
}


int MMStartReceivers(char* conns, int node_id)
{
    int i = 0;
	BackgroundWorker worker;
    char* conn_str = conns;
    char* conn_str_end = conn_str + strlen(conn_str);
	MemSet(&worker, 0, sizeof(BackgroundWorker));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS |  BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_main = pglogical_receiver_main; 
	worker.bgw_restart_time = 10; /* Wait 10 seconds for restart before crash */

    while (conn_str < conn_str_end) { 
        char* p = strchr(conn_str, ',');
        if (p == NULL) { 
            p = conn_str_end;
        }
        if (++i != node_id) {
            ReceiverArgs* ctx = (ReceiverArgs*)malloc(sizeof(ReceiverArgs));
            if (MMDatabaseName == NULL) {
                char* dbname = strstr(conn_str, "dbname=");
                char* eon;
                int len;
                Assert(dbname != NULL);
                dbname += 7;
                eon = strchr(dbname, ' ');
                len = eon - dbname;
                MMDatabaseName = (char*)malloc(len + 1);
                memcpy(MMDatabaseName, dbname, len);
                MMDatabaseName[len] = '\0';
            }
            ctx->receiver_conn_string = psprintf("replication=database %.*s", (int)(p - conn_str), conn_str);
            sprintf(ctx->receiver_slot, "mm_slot_%d", node_id);
            
            /* Worker parameter and registration */
            snprintf(worker.bgw_name, BGW_MAXLEN, "mm_worker_%d_%d", node_id, i);
            
            worker.bgw_main_arg = (Datum)ctx;
            RegisterBackgroundWorker(&worker);
        }
        conn_str = p + 1;
    }

    return i;
}

#ifndef USE_PGLOGICAL_OUTPUT
void MMExecutor(int id, void* work, size_t size)
{
    TransactionId xid = *(TransactionId*)work;
    char* stmts = (char*)work + 4;
    bool finished = false;

    MMJoinTransaction(xid);

    SetCurrentStatementStartTimestamp();               
    StartTransactionCommand();
    SPI_connect();
    PushActiveSnapshot(GetTransactionSnapshot());

    PG_TRY();
    {
        int rc = SPI_execute(stmts, false, 0);
        SPI_finish();
        PopActiveSnapshot();
        finished = true;
        if (rc != SPI_OK_INSERT && rc != SPI_OK_UPDATE && rc != SPI_OK_DELETE) {
            ereport(LOG, (errmsg("Executor %d: failed to apply transaction %u",
                                 id, xid)));
            AbortCurrentTransaction();
        } else { 
            CommitTransactionCommand();
        }
    }
    PG_CATCH();
    {
        FlushErrorState();
        if (!finished) {
            SPI_finish();
            if (ActiveSnapshotSet()) { 
                PopActiveSnapshot();
            }
        }
        AbortCurrentTransaction();
    }
    PG_END_TRY();
}
#endif
