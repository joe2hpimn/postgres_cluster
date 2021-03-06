n_nodes=3
export PATH=~/postgrespro/cluster_install/bin/:$PATH
ulimit -c unlimited
pkill -9 postgres
pkill -9 dtmd
sleep 2
rm -fr node? *.log dtm/*
conn_str=""
sep=""
for ((i=1;i<=n_nodes;i++))
do    
    port=$((5431+i))
    conn_str="$conn_str${sep}replication=database dbname=postgres host=127.0.0.1 port=$port sslmode=disable"
    sep=","
    initdb node$i
done

echo Start DTM
~/postgrespro/contrib/multimaster/dtmd/bin/dtmd -d dtm 2> dtm.log &
sleep 2

echo Start nodes
for ((i=1;i<=n_nodes;i++))
do
    port=$((5431+i))
    sed "s/5432/$port/g" < postgresql.conf.mm > node$i/postgresql.conf
    echo "multimaster.conn_strings = '$conn_str'" >> node$i/postgresql.conf
    echo "multimaster.node_id = $i" >> node$i/postgresql.conf
    cp pg_hba.conf node$i
    pg_ctl -D node$i -l node$i.log start
done

sleep 5
echo Initialize database schema
for ((i=1;i<=n_nodes;i++))
do
    port=$((5431+i))
    psql -p $port postgres -f init.sql
done

echo Done
