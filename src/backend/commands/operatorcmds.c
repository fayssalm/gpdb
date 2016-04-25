/*-------------------------------------------------------------------------
 *
 * operatorcmds.c
 *
 *	  Routines for operator manipulation commands
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/operatorcmds.c,v 1.35 2007/01/05 22:19:26 momjian Exp $
 *
 * DESCRIPTION
 *	  The "DefineFoo" routines take the parse tree and pick out the
 *	  appropriate arguments/flags, passing the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 * NOTES
 *	  These things must be defined and committed in the following order:
 *		"create function":
 *				input/output, recv/send procedures
 *		"create type":
 *				type
 *		"create operator":
 *				operators
 *
 *		Most of the parse-tree manipulation routines are defined in
 *		commands/manip.c.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catquery.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_operator.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "cdb/cdbvars.h"
#include "cdb/cdbdisp.h"

static void AlterOperatorOwner_internal(Relation rel, Oid operOid, Oid newOwnerId);

/*
 * DefineOperator
 *		this function extracts all the information from the
 *		parameter list generated by the parser and then has
 *		OperatorCreate() do all the actual work.
 *
 * 'parameters' is a list of DefElem
 */
void
DefineOperator(List *names, List *parameters,
			   Oid newOid, Oid newCommutatorOid, Oid newNegatorOid)
{
	char	   *oprName;
	Oid			oprNamespace;
	AclResult	aclresult;
	bool		canMerge = false;		/* operator merges */
	bool		canHash = false;		/* operator hashes */
	List	   *functionName = NIL;		/* function for operator */
	TypeName   *typeName1 = NULL;		/* first type name */
	TypeName   *typeName2 = NULL;		/* second type name */
	Oid			typeId1 = InvalidOid;	/* types converted to OID */
	Oid			typeId2 = InvalidOid;
	List	   *commutatorName = NIL;	/* optional commutator operator name */
	List	   *negatorName = NIL;		/* optional negator operator name */
	List	   *restrictionName = NIL;	/* optional restrict. sel. procedure */
	List	   *joinName = NIL; /* optional join sel. procedure */
	ListCell   *pl;
	Oid    opOid;

	/* Convert list of names to a name and namespace */
	oprNamespace = QualifiedNameGetCreationNamespace(names, &oprName);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(oprNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(oprNamespace));

	/*
	 * loop over the definition list and extract the information we need.
	 */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (pg_strcasecmp(defel->defname, "leftarg") == 0)
		{
			typeName1 = defGetTypeName(defel);
			if (typeName1->setof)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					errmsg("setof type not allowed for operator argument")));
		}
		else if (pg_strcasecmp(defel->defname, "rightarg") == 0)
		{
			typeName2 = defGetTypeName(defel);
			if (typeName2->setof)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					errmsg("setof type not allowed for operator argument")));
		}
		else if (pg_strcasecmp(defel->defname, "procedure") == 0)
			functionName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "commutator") == 0)
			commutatorName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "negator") == 0)
			negatorName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "restrict") == 0)
			restrictionName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "join") == 0)
			joinName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "hashes") == 0)
			canHash = defGetBoolean(defel);
		else if (pg_strcasecmp(defel->defname, "merges") == 0)
			canMerge = defGetBoolean(defel);
		/* These obsolete options are taken as meaning canMerge */
		else if (pg_strcasecmp(defel->defname, "sort1") == 0)
			canMerge = true;
		else if (pg_strcasecmp(defel->defname, "sort2") == 0)
			canMerge = true;
		else if (pg_strcasecmp(defel->defname, "ltcmp") == 0)
			canMerge = true;
		else if (pg_strcasecmp(defel->defname, "gtcmp") == 0)
			canMerge = true;
		else
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("operator attribute \"%s\" not recognized",
							defel->defname)));
	}

	/*
	 * make sure we have our required definitions
	 */
	if (functionName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("operator procedure must be specified")));

	/* Transform type names to type OIDs */
	if (typeName1)
		typeId1 = typenameTypeId(NULL, typeName1);
	if (typeName2)
		typeId2 = typenameTypeId(NULL, typeName2);

	/*
	 * now have OperatorCreate do all the work..
	 */
	opOid = OperatorCreateWithOid(oprName,		/* operator name */
				   oprNamespace,	/* namespace */
				   typeId1,		/* left type id */
				   typeId2,		/* right type id */
				   functionName,	/* function for operator */
				   commutatorName,		/* optional commutator operator name */
				   negatorName, /* optional negator operator name */
				   restrictionName,		/* optional restrict. sel. procedure */
				   joinName,	/* optional join sel. procedure name */
				   canMerge,	/* operator merges */
				   canHash,	/* operator hashes */
				   newOid,
				   &newCommutatorOid,
				   &newNegatorOid);

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		DefineStmt * stmt = makeNode(DefineStmt);
		stmt->kind = OBJECT_OPERATOR;
		stmt->oldstyle = false;
		stmt->defnames = names;
		stmt->args = NIL;
		stmt->definition = parameters;
		stmt->newOid = opOid;
		stmt->commutatorOid = newCommutatorOid;
		stmt->negatorOid = newNegatorOid;
		stmt->arrayOid = InvalidOid;
		CdbDispatchUtilityStatement((Node *) stmt, "DefineOperator");
	}
}


/*
 * RemoveOperator
 *		Deletes an operator.
 */
void
RemoveOperator(RemoveFuncStmt *stmt)
{
	List	   *operatorName = stmt->name;
	TypeName   *typeName1 = (TypeName *) linitial(stmt->args);
	TypeName   *typeName2 = (TypeName *) lsecond(stmt->args);
	Oid			operOid;
	Oid			operNsp;
	int			fetchCount = 0;
	ObjectAddress object;

	Assert(list_length(stmt->args) == 2);
	operOid = LookupOperNameTypeNames(NULL, operatorName,
									  typeName1, typeName2,
									  stmt->missing_ok, -1);

	if (stmt->missing_ok && !OidIsValid(operOid))
	{
		ereport(NOTICE,
				(errmsg("operator %s does not exist, skipping",
						NameListToString(operatorName))));
		return;
	}

	operNsp = caql_getoid_plus(
			NULL,
			&fetchCount,
			NULL,
			cql("SELECT oprnamespace FROM pg_operator "
				" WHERE oid = :1 ",
				ObjectIdGetDatum(operOid)));

	if (0 == fetchCount) /* should not happen */
		elog(ERROR, "cache lookup failed for operator %u", operOid);

	/* Permission check: must own operator or its namespace */
	if (!pg_oper_ownercheck(operOid, GetUserId()) &&
		!pg_namespace_ownercheck(operNsp,
								 GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_OPER,
					   NameListToString(operatorName));


	/*
	 * Do the deletion
	 */
	object.classId = OperatorRelationId;
	object.objectId = operOid;
	object.objectSubId = 0;

	performDeletion(&object, stmt->behavior);
	
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		CdbDispatchUtilityStatement((Node *) stmt, "RemoveOperator");
	}
}

/*
 * Guts of operator deletion.
 */
void
RemoveOperatorById(Oid operOid)
{
	if (0 == caql_getcount(
				NULL,
				cql("DELETE FROM pg_operator "
					" WHERE oid = :1 ",
					ObjectIdGetDatum(operOid))))
	{
		/* should not happen */
		elog(ERROR, "cache lookup failed for operator %u", operOid);
	}
}

void
AlterOperatorOwner_oid(Oid operOid, Oid newOwnerId)
{
	Relation	rel;

	rel = heap_open(OperatorRelationId, RowExclusiveLock);

	AlterOperatorOwner_internal(rel, operOid, newOwnerId);

	heap_close(rel, NoLock);
}

/*
 * change operator owner
 */
void
AlterOperatorOwner(List *name, TypeName *typeName1, TypeName *typeName2,
				   Oid newOwnerId)
{
	Oid			operOid;
	Relation	rel;

	rel = heap_open(OperatorRelationId, RowExclusiveLock);

	operOid = LookupOperNameTypeNames(NULL, name,
									  typeName1, typeName2,
									  false, -1);

	AlterOperatorOwner_internal(rel, operOid, newOwnerId);

	heap_close(rel, NoLock);
}

static void
AlterOperatorOwner_internal(Relation rel, Oid operOid, Oid newOwnerId)
{
	HeapTuple	tup;
	AclResult	aclresult;
	Form_pg_operator oprForm;
	cqContext	cqc;
	cqContext  *pcqCtx;

	Assert(RelationGetRelid(rel) == OperatorRelationId);

	pcqCtx = caql_addrel(cqclr(&cqc), rel);

	tup = caql_getfirst(
			pcqCtx,
			cql("SELECT * FROM pg_operator "
				" WHERE oid = :1 "
				" FOR UPDATE ",
				ObjectIdGetDatum(operOid)));

	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for operator %u", operOid);

	oprForm = (Form_pg_operator) GETSTRUCT(tup);

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is for dump restoration purposes.
	 */
	if (oprForm->oprowner != newOwnerId)
	{
		/* Superusers can always do it */
		if (!superuser())
		{
			/* Otherwise, must be owner of the existing object */
			if (!pg_oper_ownercheck(operOid, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_OPER,
							   NameStr(oprForm->oprname));

			/* Must be able to become new owner */
			check_is_member_of_role(GetUserId(), newOwnerId);

			/* New owner must have CREATE privilege on namespace */
			aclresult = pg_namespace_aclcheck(oprForm->oprnamespace,
											  newOwnerId,
											  ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
							   get_namespace_name(oprForm->oprnamespace));
		}

		/*
		 * Modify the owner --- okay to scribble on tup because it's a copy
		 */
		oprForm->oprowner = newOwnerId;

		caql_update_current(pcqCtx, tup); /* implicit update of index as well*/

		/* Update owner dependency reference */
		changeDependencyOnOwner(OperatorRelationId, operOid, newOwnerId);
	}

	heap_freetuple(tup);
}
