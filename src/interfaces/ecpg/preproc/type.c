#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "type.h"
#include "extern.h"

/* malloc + error check */
void *
mm_alloc(size_t size)
{
	void	   *ptr = malloc(size);

	if (ptr == NULL)
	{
		fprintf(stderr, "Out of memory\n");
		exit(1);
	}

	return (ptr);
}

/* realloc + error check */
void *
mm_realloc(void *ptr, size_t size)
{
	ptr = realloc(ptr, size);

	if (ptr == NULL)
	{
		fprintf(stderr, "Out of memory\n");
		exit(1);
	}

	return (ptr);
}

/* Constructors
   Yes, I mostly write c++-code
 */

/* The NAME argument is copied. The type argument is preserved as a pointer. */
struct ECPGstruct_member *
ECPGmake_struct_member(char *name, struct ECPGtype * type, struct ECPGstruct_member ** start)
{
	struct ECPGstruct_member *ptr,
			   *ne =
	(struct ECPGstruct_member *) mm_alloc(sizeof(struct ECPGstruct_member));

	ne->name = strdup(name);
	ne->typ = type;
	ne->next = NULL;

	for (ptr = *start; ptr && ptr->next; ptr = ptr->next);

	if (ptr)
		ptr->next = ne;
	else
		*start = ne;
	return ne;
}

struct ECPGtype *
ECPGmake_simple_type(enum ECPGttype typ, long siz)
{
	struct ECPGtype *ne = (struct ECPGtype *) mm_alloc(sizeof(struct ECPGtype));

	ne->typ = typ;
	ne->size = siz;
	ne->u.element = 0;

	return ne;
}

struct ECPGtype *
ECPGmake_varchar_type(enum ECPGttype typ, long siz)
{
	struct ECPGtype *ne = ECPGmake_simple_type(typ, 1);

	ne->size = siz;

	return ne;
}

struct ECPGtype *
ECPGmake_array_type(struct ECPGtype * typ, long siz)
{
	struct ECPGtype *ne = ECPGmake_simple_type(ECPGt_array, siz);

	ne->size = siz;
	ne->u.element = typ;

	return ne;
}

struct ECPGtype *
ECPGmake_struct_type(struct ECPGstruct_member * rm)
{
	struct ECPGtype *ne = ECPGmake_simple_type(ECPGt_struct, 1);

	ne->u.members = rm;

	return ne;
}


/* Dump a type.
   The type is dumped as:
   type-tag <comma>				   - enum ECPGttype
   reference-to-variable <comma>   - void *
   size <comma>					   - long size of this field (if varchar)
   arrsize <comma>				   - long number of elements in the arr
   offset <comma>				   - offset to the next element
   Where:
   type-tag is one of the simple types or varchar.
   reference-to-variable can be a reference to a struct element.
   arrsize is the size of the array in case of array fetches. Otherwise 0.
   size is the maxsize in case it is a varchar. Otherwise it is the size of
   the variable (required to do array fetches of structs).
 */
void
ECPGdump_a_simple(FILE *o, const char *name, enum ECPGttype typ,
				  long varcharsize,
				  long arrsiz, const char *siz, const char *prefix);
void
ECPGdump_a_struct(FILE *o, const char *name, const char *ind_name, long arrsiz,
		  struct ECPGtype * typ, struct ECPGtype * ind_typ, const char *offset, const char *prefix, const char * ind_prefix);


void
ECPGdump_a_type(FILE *o, const char *name, struct ECPGtype * typ, const char *ind_name, struct ECPGtype * ind_typ, const char *prefix, const char *ind_prefix)
{
	if (ind_typ == NULL)
	{
		ind_typ = &ecpg_no_indicator;
		ind_name = "no_indicator";
	}
	
	if (IS_SIMPLE_TYPE(typ->typ))
	{
		ECPGdump_a_simple(o, name, typ->typ, typ->size, 0, 0, prefix);
                ECPGdump_a_simple(o, ind_name, ind_typ->typ, ind_typ->size, 0, 0, ind_prefix);
	}
	else if (typ->typ == ECPGt_array)
	{
		if (IS_SIMPLE_TYPE(typ->u.element->typ))
		{
			ECPGdump_a_simple(o, name, typ->u.element->typ,
							  typ->u.element->size, typ->size, 0, prefix);
                	ECPGdump_a_simple(o, ind_name, ind_typ->u.element->typ,
							  ind_typ->u.element->size, ind_typ->size, 0, prefix);
		}				  
		else if (typ->u.element->typ == ECPGt_array)
		{
			abort();			/* Array of array, */
		}
		else if (typ->u.element->typ == ECPGt_struct)
		{
			/* Array of structs. */
			ECPGdump_a_struct(o, name, ind_name, typ->size, typ->u.element, ind_typ->u.element, 0, prefix, ind_prefix);
		}
		else
		{
			abort();
		}
	}
	else if (typ->typ == ECPGt_struct)
	{
		ECPGdump_a_struct(o, name, ind_name, 0, typ, ind_typ, 0, prefix, ind_prefix);
	}
	else
	{
		abort();
	}
}


/* If siz is NULL, then the offset is 0, if not use siz as a
   string, it represents the offset needed if we are in an array of structs. */
void
ECPGdump_a_simple(FILE *o, const char *name, enum ECPGttype typ,
				  long varcharsize,
				  long arrsiz,
				  const char *siz,
				  const char *prefix
				  )
{
	switch (typ)
	{
			case ECPGt_char:
			if (varcharsize == 0)		/* pointer */
				fprintf(o, "\n\tECPGt_char,(%s%s),%ldL,%ldL,%s, ", prefix ? prefix : "", name, varcharsize, arrsiz,
						siz == NULL ? "sizeof(char)" : siz);
			else
				fprintf(o, "\n\tECPGt_char,&(%s%s),%ldL,%ldL,%s, ", prefix ? prefix : "", name, varcharsize, arrsiz,
						siz == NULL ? "sizeof(char)" : siz);
			break;
		case ECPGt_unsigned_char:
			if (varcharsize == 0)		/* pointer */
				fprintf(o, "\n\tECPGt_unsigned_char,(%s%s),%ldL,%ldL,%s, ", prefix ? prefix : "", name, varcharsize, arrsiz,
						siz == NULL ? "sizeof(char)" : siz);
			else
				fprintf(o, "\n\tECPGt_unsigned_char,&(%s%s),%ldL,%ldL,%s, ", prefix ? prefix : "", name, varcharsize, arrsiz,
						siz == NULL ? "sizeof(unsigned char)" : siz);
			break;
		case ECPGt_short:
			fprintf(o, "\n\tECPGt_short,&(%s%s),0L,%ldL,%s, ", prefix ? prefix : "", name, arrsiz,
					siz == NULL ? "sizeof(short)" : siz);
			break;
		case ECPGt_unsigned_short:
			fprintf(o,
					"\n\tECPGt_unsigned_short,&(%s%s),0L,%ldL,%s, ", prefix ? prefix : "", name, arrsiz,
					siz == NULL ? "sizeof(unsigned short)" : siz);
			break;
		case ECPGt_int:
			fprintf(o, "\n\tECPGt_int,&(%s%s),0L,%ldL,%s, ", prefix ? prefix : "", name, arrsiz,
					siz == NULL ? "sizeof(int)" : siz);
			break;
		case ECPGt_unsigned_int:
			fprintf(o, "\n\tECPGt_unsigned_int,&(%s%s),0L,%ldL,%s, ", prefix ? prefix : "", name, arrsiz,
					siz == NULL ? "sizeof(unsigned int)" : siz);
			break;
		case ECPGt_long:
			fprintf(o, "\n\tECPGt_long,&(%s%s),0L,%ldL,%s, ", prefix ? prefix : "", name, arrsiz,
					siz == NULL ? "sizeof(long)" : siz);
			break;
		case ECPGt_unsigned_long:
			fprintf(o, "\n\tECPGt_unsigned_int,&(%s%s),0L,%ldL,%s, ", prefix ? prefix : "", name, arrsiz,
					siz == NULL ? "sizeof(unsigned int)" : siz);
			break;
		case ECPGt_float:
			fprintf(o, "\n\tECPGt_float,&(%s%s),0L,%ldL,%s, ", prefix ? prefix : "", name, arrsiz,
					siz == NULL ? "sizeof(float)" : siz);
			break;
		case ECPGt_double:
			fprintf(o, "\n\tECPGt_double,&(%s%s),0L,%ldL,%s, ", prefix ? prefix : "", name, arrsiz,
					siz == NULL ? "sizeof(double)" : siz);
			break;
		case ECPGt_bool:
			fprintf(o, "\n\tECPGt_bool,&(%s%s),0L,%ldL,%s, ", prefix ? prefix : "", name, arrsiz,
					siz == NULL ? "sizeof(bool)" : siz);
			break;
		case ECPGt_varchar:
		case ECPGt_varchar2:
			if (siz == NULL)
				fprintf(o, "\n\tECPGt_varchar,&(%s%s),%ldL,%ldL,sizeof(struct varchar_%s), ",
						prefix ? prefix : "", name,
						varcharsize,
						arrsiz, name);
			else
				fprintf(o, "\n\tECPGt_varchar,&(%s%s),%ldL,%ldL,%s, ",
						prefix ? prefix : "", name,
						varcharsize,
						arrsiz, siz);
			break;
		case ECPGt_NO_INDICATOR: /* no indicator */
		        fprintf(o, "\n\tECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ");
		        break;
		default:
			abort();
	}
	                            
}


/* Penetrate a struct and dump the contents. */
void
ECPGdump_a_struct(FILE *o, const char *name, const char * ind_name, long arrsiz, struct ECPGtype * typ, struct ECPGtype * ind_typ, const char *offsetarg, const char *prefix, const char *ind_prefix)
{

	/*
	 * If offset is NULL, then this is the first recursive level. If not
	 * then we are in a struct in a struct and the offset is used as
	 * offset.
	 */
	struct ECPGstruct_member *p, *ind_p = NULL;
	char		obuf[BUFSIZ];
	char		pbuf[BUFSIZ], ind_pbuf[BUFSIZ];
	const char *offset;

	if (offsetarg == NULL)
	{
		sprintf(obuf, "sizeof(%s)", name);
		offset = obuf;
	}
	else
	{
		offset = offsetarg;
	}

	sprintf(pbuf, "%s%s.", prefix ? prefix : "", name);
	prefix = pbuf;
	
	sprintf(ind_pbuf, "%s%s.", ind_prefix ? ind_prefix : "", ind_name);
	ind_prefix = ind_pbuf;

	if (ind_typ != NULL) ind_p = ind_typ->u.members;
	for (p = typ->u.members; p; p = p->next)
	{
		ECPGdump_a_type(o, p->name, p->typ, (ind_p != NULL) ? ind_p->name : NULL, (ind_p != NULL) ? ind_p->typ : NULL, prefix, ind_prefix);
		if (ind_p != NULL) ind_p = ind_p->next;
	}
}


/* Freeing is not really that important. Since we throw away the process
   anyway. Lets implement that last! */

/* won't work anymore because a list of members may appear in several locations */
/*void
ECPGfree_struct_member(struct ECPGstruct_member * rm)
{
	while (rm)
	{
		struct ECPGstruct_member *p = rm;

		rm = rm->next;
		free(p->name);
		free(p);
	}
}*/

void
ECPGfree_type(struct ECPGtype * typ)
{
	if (!IS_SIMPLE_TYPE(typ->typ))
	{
		if (typ->typ == ECPGt_array)
		{
			if (IS_SIMPLE_TYPE(typ->u.element->typ))
				free(typ->u.element);
			else if (typ->u.element->typ == ECPGt_array)
				abort();		/* Array of array, */
			else if (typ->u.element->typ == ECPGt_struct)
				/* Array of structs. */
				free(typ->u.members);
				/* ECPGfree_struct_member(typ->u.members);*/
			else
				abort();
		}
		else if (typ->typ == ECPGt_struct)
		{
			free(typ->u.members);
			/* ECPGfree_struct_member(typ->u.members);*/
		}
		else
		{
			abort();
		}
	}
	free(typ);
}
