/*	
 *	Author:	Jeffrey O. Hill
 *		hill@luke.lanl.gov
 *		(505) 665 1831
 *	Date:	5-88
 *
 *	Experimental Physics and Industrial Control System (EPICS)
 *
 *	Copyright 1991, the Regents of the University of California,
 *	and the University of Chicago Board of Governors.
 *
 *	This software was produced under  U.S. Government contracts:
 *	(W-7405-ENG-36) at the Los Alamos National Laboratory,
 *	and (W-31-109-ENG-38) at Argonne National Laboratory.
 *
 *	Initial development by:
 *		The Controls and Automation Group (AT-8)
 *		Ground Test Accelerator
 *		Accelerator Technology Division
 *		Los Alamos National Laboratory
 *
 *	Co-developed with
 *		The Controls and Computing Group
 *		Accelerator Systems Division
 *		Advanced Photon Source
 *		Argonne National Laboratory
 *
 * 	Modification Log:
 * 	-----------------
 *	.01 joh 012290 	placed break in ca_cancel_event search so entire 
 *			list is not searched.
 *	.02 joh	011091	added missing break to error message send 
 *			chix select switch
 *	.03 joh	071291	now timestanmps channel in use block
 *	.04 joh	071291	code for IOC_CLAIM_CIU command
 *	.05 joh 082691  use db_post_single_event() instead of read_reply()
 *			to avoid deadlock condition between the client
 *			and the server.
 *	.06 joh	110491	lock added for IOC_CLAIM_CIU command
 *	.07 joh	021292	Better diagnostics
 *	.08 joh	021492	use lstFind() to verify chanel in clear_channel()
 *	.09 joh 031692	dont send exeception to the client if bad msg
 *			detected- just disconnect instead
 *	.10 joh	050692	added new routine - cas_send_heartbeat()
 *	.11 joh 062492	dont allow flow control to turn off gets
 *	.12 joh 090893	converted pointer to server id	
 *	.13 joh 091493	made events on action a subroutine for debugging
 */

static char *sccsId = "$Id$";

#include <vxWorks.h>
#include <taskLib.h>
#include <types.h>
#include <in.h>
#include <logLib.h>
#include <tickLib.h>
#include <stdioLib.h>

#include <ellLib.h>
#include <db_access.h>
#include <task_params.h>
#include <server.h>
#include <caerr.h>

static struct extmsg nill_msg;

	
#define	RECORD_NAME(PADDR) (((struct db_addr *)(PADDR))->precord)

LOCAL void clear_channel_reply(
struct extmsg *mp,
struct client  *client
);

LOCAL void event_cancel_reply(
struct extmsg *mp,
struct client  *client
);

LOCAL void read_reply(
struct event_ext *pevext,
struct db_addr *paddr,
int             hold,	/* more on the way if true */
void		*pfl
);

LOCAL void read_sync_reply(
struct extmsg *mp,
struct client  *client
);

LOCAL void build_reply(
struct extmsg *mp,
struct client  *client
);

LOCAL void search_fail_reply(
struct extmsg 	*mp,
struct client  	*client
);

LOCAL void send_err(
struct extmsg  *curp,
int            status,
struct client  *client,
char           *footnote
);

LOCAL void log_header(
struct extmsg 	*mp,
int		mnum
);

LOCAL void event_add_action(
struct extmsg *mp,
struct client  *client
);

LOCAL void logBadId(
struct client *client, 
struct extmsg *mp
);

LOCAL struct channel_in_use *MPTOPCIU(
struct extmsg *mp
);

LOCAL void events_on_action(
struct client  *client
);

LOCAL unsigned long	bucketID;


/*
 * CAMESSAGE()
 *
 *
 */
int camessage(
struct client  *client,
struct message_buffer *recv
)
{
	int            		nmsg = 0;
	unsigned		msgsize;
	unsigned		bytes_left;
	int             	status;
	struct extmsg 		*mp;
	struct channel_in_use 	*pciu;

	if(!pCaBucket){
		pCaBucket = bucketCreate(NBBY*sizeof(mp->m_cid));
		if(!pCaBucket){
			return ERROR;
		}
	}

	if (CASDEBUG > 2){
		logMsg(	"CAS: Parsing %d(decimal) bytes\n", 
			recv->cnt,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL);
	}

	bytes_left = recv->cnt;
	while (bytes_left) {

		if(bytes_left < sizeof(*mp))
			return OK;

		mp = (struct extmsg *) &recv->buf[recv->stk];

		msgsize = mp->m_postsize + sizeof(*mp);

		if(msgsize > bytes_left) 
			return OK;

		nmsg++;

		if (CASDEBUG > 2)
			log_header(mp, nmsg);

		switch (mp->m_cmmd) {
		case IOC_NOOP:	/* verify TCP */
			break;

		case IOC_EVENT_ADD:
			event_add_action(mp, client);
			break;

		case IOC_EVENT_CANCEL:
			event_cancel_reply(mp, client);
			break;

		case IOC_CLEAR_CHANNEL:
			clear_channel_reply(mp, client);
			break;

		case IOC_READ_NOTIFY:
		case IOC_READ:
		{
			struct event_ext evext;

			pciu = MPTOPCIU(mp);
			if(!pciu){
				logBadId(client, mp);
				break;
			}

			evext.mp = mp;
			evext.pciu = pciu;
			evext.send_lock = TRUE;
			evext.get = TRUE;
			if (mp->m_count == 1)
				evext.size = dbr_size[mp->m_type];
			else
				evext.size = (mp->m_count - 1) * 
					dbr_value_size[mp->m_type] +
					dbr_size[mp->m_type];

			/*
			 * Arguments to this routine organized in
			 * favor of the standard db event calling
			 * mechanism-  routine(userarg, paddr). See
			 * events added above.
			 * 
			 * Hold argument set true so the send message
			 * buffer is not flushed once each call.
			 */
			read_reply(&evext, &pciu->addr, TRUE, NULL);
			break;
		}
		case IOC_SEARCH:
		case IOC_BUILD:
			build_reply(mp, client);
			break;
		case IOC_WRITE:
			pciu = MPTOPCIU(mp);
			if(!pciu){
				logBadId(client, mp);
				break;
			}
			status = db_put_field(
					      &pciu->addr,
					      mp->m_type,
					      mp + 1,
					      mp->m_count
				);
			if (status < 0) {
				LOCK_CLIENT(client);
				send_err(
					mp, 
					ECA_PUTFAIL, 
					client, 
					RECORD_NAME(pciu));
				UNLOCK_CLIENT(client);
			}
			break;

		case IOC_EVENTS_ON:
			events_on_action(client);
			break;

		case IOC_EVENTS_OFF:
			client->eventsoff = TRUE;
			break;

		case IOC_READ_SYNC:
			read_sync_reply(mp, client);
			break;
		case IOC_CLAIM_CIU:
			/*
			 * remove channel in use block from
			 * the UDP client where it could time
			 * out and place it on the client
			 * who is claiming it
			 */

			/*
			 * clients which dont claim their 
			 * channel in use block prior to
			 * timeout must reconnect
			 */
			pciu = MPTOPCIU(mp);
			if(pciu?pciu->client!=prsrv_cast_client:TRUE){
       				free_client(client);
				logMsg("CAS: client timeout disconnect\n",
					NULL,
					NULL,
					NULL,
					NULL,
					NULL,
					NULL);
				exit(0);
			}

			LOCK_CLIENT(prsrv_cast_client);
			ellDelete(
				&prsrv_cast_client->addrq, 
				&pciu->node);
			UNLOCK_CLIENT(prsrv_cast_client);
			pciu->client = client;
			LOCK_CLIENT(client);
			ellAdd(&client->addrq, &pciu->node);
			UNLOCK_CLIENT(client);
			break;

		default:
			logMsg("CAS: bad msg detected\n",
				NULL,
				NULL,
				NULL,
				NULL,
				NULL,
				NULL);
			log_header(mp, nmsg);
#if 0	
			/* 
			 *	most clients dont recover
			 *	from this
			 */
			LOCK_CLIENT(client);
			send_err(mp, ECA_INTERNAL, client, "Invalid Msg");
			UNLOCK_CLIENT(client);
#endif
			/*
			 * returning ERROR here disconnects
			 * the client with the bad message
			 */
			logMsg("CAS: forcing disconnect ...\n",
				NULL,
				NULL,
				NULL,
				NULL,
				NULL,
				NULL);
			return ERROR;
		}

		recv->stk += msgsize;
		bytes_left = recv->cnt - recv->stk;
	}


	return OK;
}


/*
 *
 * events_on_action()
 *
 */
LOCAL void events_on_action(
struct client  *client
)
{
	struct channel_in_use 	*pciu;
	struct event_ext *pevext;
	struct event_ext evext;

	client->eventsoff = FALSE;

	LOCK_CLIENT(client);

	pciu = (struct channel_in_use *) 
		client->addrq.node.next;
	while (pciu) {
		pevext = (struct event_ext *) 
			pciu->eventq.node.next;
		while (pevext){

			if (pevext->modified) {
				evext = *pevext;
				evext.send_lock = FALSE;
				evext.get = TRUE;
				read_reply(
					&evext, 
					&pciu->addr, 
					TRUE, 
					NULL);
				pevext->modified = FALSE;
			}
			pevext = (struct event_ext *)
				pevext->node.next;
		}

		pciu = (struct channel_in_use *)
			pciu->node.next;
	}
	UNLOCK_CLIENT(client);
}


/*
 *
 * event_add_action()
 *
 */
LOCAL void event_add_action(
struct extmsg *mp,
struct client  *client
)
{
	struct channel_in_use *pciu;
	struct event_ext *pevext;
	int		status;

	pciu = MPTOPCIU(mp);
	if(!pciu){
		logBadId(client, mp);
		return;
	}

	FASTLOCK(&rsrv_free_eventq_lck);
	pevext = (struct event_ext *) 
		ellGet(&rsrv_free_eventq);
	FASTUNLOCK(&rsrv_free_eventq_lck);
	if (!pevext) {
		int size = db_sizeof_event_block() 
				+ sizeof(*pevext);
		pevext =
			(struct event_ext *) malloc(size);
		if (!pevext) {
			LOCK_CLIENT(client);
			send_err(
				mp,
				ECA_ALLOCMEM, 
				client, 
				RECORD_NAME(pciu));
			UNLOCK_CLIENT(client);
			return;
		}
	}
	pevext->msg = *mp;
	pevext->mp = &pevext->msg;	/* for speed- see
					 * IOC_READ */
	pevext->pciu = pciu;
	pevext->send_lock = TRUE;
	pevext->size = (mp->m_count - 1) 
		* dbr_value_size[mp->m_type] +
		dbr_size[mp->m_type];
	pevext->get = FALSE;

	status = db_add_event(client->evuser,
			      &pciu->addr,
			      read_reply,
			      pevext,
	   (unsigned) ((struct monops *) mp)->m_info.m_mask,
  	   (struct event_block *)(pevext+1));
	if (status == ERROR) {
		LOCK_CLIENT(client);
		send_err(
			mp, 
			ECA_ADDFAIL, 
			client, 
			RECORD_NAME(pciu));
		UNLOCK_CLIENT(client);
		FASTLOCK(&rsrv_free_eventq_lck);
		ellAdd((ELLLIST *)&rsrv_free_eventq, (ELLNODE *)pevext);
		FASTUNLOCK(&rsrv_free_eventq_lck);
		return;
	}

	/*
	 * Only add to the list if we can get enough
	 * memory. If not, attempts to delete it 
	 * from the client will cause a warning message
	 * to be printed since it will not be found on
	 * the list.
	 */
	ellAdd(	(ELLLIST *)&pciu->eventq, 
		(ELLNODE *)pevext);

	/*
	 * always send it once at event add
	 */
	/*
	 * if the client program issues many monitors
	 * in a row then I recv when the send side
	 * of the socket would block. This prevents
	 * a application program initiated deadlock.
	 *
	 * However when I am reconnecting I reissue 
	 * the monitors and I could get deadlocked.
	 * The client is blocked sending and the server
	 * task for the client is blocked sending in
	 * this case. I cant check the recv part of the
	 * socket in the client since I am still handling an
	 * outstanding recv ( they must be processed in order).
	 * I handle this problem in the server by using
	 * post_single_event() below instead of calling
	 * read_reply() in this module. This is a complete
	 * fix since a monitor setup is the only request
	 * soliciting a reply in the client which is 
	 * issued from inside of service.c (from inside
	 * of the part of the ca client which services
	 * messages sent by the server).
	 */

	db_post_single_event((struct event_block *)(pevext+1));

	return;
}



/*
 *
 *	clear_channel_reply()
 *
 *
 */
LOCAL void clear_channel_reply(
struct extmsg *mp,
struct client  *client
)
{
        FAST struct extmsg *reply;
        FAST struct event_ext *pevext;
        struct channel_in_use *pciu;
        FAST int        status;

        /*
         *
         * Verify the channel
         *
         */
	pciu = MPTOPCIU(mp);
	if(pciu?pciu->client!=client:TRUE){
		logBadId(client, mp);
		return;
	}

        while (pevext = (struct event_ext *) ellGet(&pciu->eventq)) {

		status = db_cancel_event((struct event_block *)(pevext+1));
		if (status == ERROR){
			taskSuspend(0);
		}
		FASTLOCK(&rsrv_free_eventq_lck);
		ellAdd((ELLLIST *)&rsrv_free_eventq, (ELLNODE *)pevext);
		FASTUNLOCK(&rsrv_free_eventq_lck);
        }


	/*
	 * send delete confirmed message
	 */
	LOCK_CLIENT(client);
	reply = (struct extmsg *) ALLOC_MSG(client, 0);
	if (!reply) {
		UNLOCK_CLIENT(client);
		taskSuspend(0);
	}
	*reply = *mp;

	END_MSG(client);
	ellDelete((ELLLIST *)&client->addrq, (ELLNODE *)pciu);
	UNLOCK_CLIENT(client);

	FASTLOCK(&rsrv_free_addrq_lck);
	status = bucketRemoveItem(pCaBucket, pciu->sid, pciu);
	if(status != BUCKET_SUCCESS){
		logBadId(client, mp);
	}
	ellAdd((ELLLIST *)&rsrv_free_addrq, (ELLNODE *)pciu);
	FASTUNLOCK(&rsrv_free_addrq_lck);

	return;
}




/*
 *
 *	event_cancel_reply()
 *
 *
 * Much more efficient now since the event blocks hang off the channel in use
 * blocks not all together off the client block.
 */
LOCAL void event_cancel_reply(
struct extmsg *mp,
struct client  *client
)
{
	struct extmsg *reply;
	struct event_ext *pevext;
	ELLLIST           *peventq; 
	int        status;
        struct channel_in_use *pciu;

        /*
         *
         * Verify the channel
         *
         */
	pciu = MPTOPCIU(mp);
	if(pciu?pciu->client!=client:TRUE){
		logBadId(client, mp);
		return;
	}
	peventq = &pciu->eventq;

	for (pevext = (struct event_ext *) peventq->node.next;
	     pevext;
	     pevext = (struct event_ext *) pevext->node.next)
		if (pevext->msg.m_available == mp->m_available) {
			status = db_cancel_event((struct event_block *)(pevext+1));
			if (status == ERROR)
				taskSuspend(0);
			ellDelete((ELLLIST *)peventq, (ELLNODE *)pevext);

			/*
			 * send delete confirmed message
			 */
			LOCK_CLIENT(client);
			reply = (struct extmsg *) ALLOC_MSG(client, 0);
			if (!reply) {
				UNLOCK_CLIENT(client);
				taskSuspend(0);
			}
			*reply = pevext->msg;
			reply->m_postsize = 0;

			END_MSG(client);
			UNLOCK_CLIENT(client);

			FASTLOCK(&rsrv_free_eventq_lck);
			ellAdd((ELLLIST *)&rsrv_free_eventq, (ELLNODE *)pevext);
			FASTUNLOCK(&rsrv_free_eventq_lck);

			return;
		}
	/*
	 * Not Found- return an error message
	 */
	LOCK_CLIENT(client);
	send_err(mp, ECA_BADMONID, client, NULL);
	UNLOCK_CLIENT(client);

	return;
}



/*
 *
 *	read_reply()
 *
 *
 */
LOCAL void read_reply(
struct event_ext *pevext,
struct db_addr *paddr,
int             hold,	/* more on the way if true */
void		*pfl
)
{
	struct extmsg *mp = pevext->mp;
	struct client *client = pevext->pciu->client;
        struct channel_in_use *pciu = pevext->pciu;
	struct extmsg *reply;
	int        status;
	int        strcnt;


	/*
	 * If flow control is on set modified and send for later
 	 * (only if this is not a get)
	 */
	if (client->eventsoff && !pevext->get) {
		pevext->modified = TRUE;
		return;
	}
	if (pevext->send_lock)
		LOCK_CLIENT(client);

	reply = (struct extmsg *) ALLOC_MSG(client, pevext->size);
	if (!reply) {
		send_err(mp, ECA_TOLARGE, client, RECORD_NAME(paddr));
		if (pevext->send_lock)
			UNLOCK_CLIENT(client);
		return;
	}
	*reply = *mp;
	reply->m_postsize = pevext->size;
	reply->m_cid = (unsigned long)pciu->cid;
	status = db_get_field(
			      paddr,
			      mp->m_type,
			      reply + 1,
			      mp->m_count,
			      pfl);
	if (status < 0) {
		send_err(mp, ECA_GETFAIL, client, RECORD_NAME(paddr));
		log_header(mp, 0);
	}
	else{
		/*
		 * force string message size to be the true size rounded to even
		 * boundary
		 */
		if (mp->m_type == DBR_STRING && mp->m_count == 1) {
			/* add 1 so that the string terminator will be shipped */
			strcnt = strlen((char *)(reply + 1)) + 1;
			reply->m_postsize = strcnt;
		}
		END_MSG(client);
	
		/*
		 * Ensures timely response for events, but does que 
		 * them up like db requests when the OPI does not keep up.
		 */
		if (!hold)
			cas_send_msg(client,FALSE);
	}

	if (pevext->send_lock)
		UNLOCK_CLIENT(client);

	return;
}


/*
 *
 *	read_sync_reply()
 *
 *
 */
LOCAL void read_sync_reply(
struct extmsg *mp,
struct client  *client
)
{
	FAST struct extmsg *reply;

	LOCK_CLIENT(client);
	reply = (struct extmsg *) ALLOC_MSG(client, 0);
	if (!reply)
		taskSuspend(0);

	*reply = *mp;

	END_MSG(client);

	UNLOCK_CLIENT(client);

	return;
}


/*
 *
 *	build_reply()
 *
 *
 */
LOCAL void build_reply(
struct extmsg *mp,
struct client  *client
)
{
	ELLLIST			*addrq = &client->addrq;
	struct extmsg 		*search_reply;
	struct extmsg 		*get_reply;
	int        		status;
	struct db_addr  	tmp_addr;
	struct channel_in_use 	*pchannel;
	unsigned long		sid;


	/* Exit quickly if channel not on this node */
	status = db_name_to_addr(
			mp->m_cmmd == IOC_BUILD ? mp + 2 : mp + 1, 
			&tmp_addr);
	if (status < 0) {
		if (CASDEBUG > 2)
			logMsg(	"CAS: Lookup for channel \"%s\" failed\n", 
				(int)(mp+1),
				NULL,
				NULL,
				NULL,
				NULL,
				NULL);
		if (mp->m_type == DOREPLY)
			search_fail_reply(mp, client);
		return;
	}

	/* get block off free list if possible */
	FASTLOCK(&rsrv_free_addrq_lck);
	pchannel = (struct channel_in_use *) ellGet(&rsrv_free_addrq);
	FASTUNLOCK(&rsrv_free_addrq_lck);
	if (!pchannel) {
		pchannel = (struct channel_in_use *) calloc(1, sizeof(*pchannel));
		if (!pchannel) {
			LOCK_CLIENT(client);
			send_err(mp, ECA_ALLOCMEM, client, RECORD_NAME(&tmp_addr));
			UNLOCK_CLIENT(client);
			return;
		}
	}
	pchannel->ticks_at_creation = tickGet();
	pchannel->addr = tmp_addr;
	pchannel->client = client;
	pchannel->cid = mp->m_cid;

	/*
	 * allocate a server id and enter the channel pointer
	 * in the table
	 */
	FASTLOCK(&rsrv_free_addrq_lck);
	sid = bucketID++;
	pchannel->sid = sid;
	status = bucketAddItem(pCaBucket, sid, pchannel);
	FASTUNLOCK(&rsrv_free_addrq_lck);
	if(status!=BUCKET_SUCCESS){
		LOCK_CLIENT(client);
		send_err(mp, ECA_ALLOCMEM, client, "No room for hash table");
		UNLOCK_CLIENT(client);
		free(pchannel);
		return;
	}

	/*
	 * UDP reliability schemes rely on both msgs in same reply Therefore
	 * the send buffer locked while both messages are placed
	 */
	LOCK_CLIENT(client);

	/* store the addr block in a Q so it can be deallocated */
	ellAdd((ELLLIST *)addrq, (ELLNODE *)pchannel);

	if (mp->m_cmmd == IOC_BUILD) {
		FAST short      type = (mp + 1)->m_type;
		FAST unsigned int count = (mp + 1)->m_count;
		FAST unsigned int size;

		/*
		 * must be large enough to hold both the search and the build-get
		 * reply in one UDP message. Hence the extra sizeof(*mp) added 
		 * in below.
		 */
		size = 	sizeof(*mp) + 		/* search reply hdr size 	*/
			sizeof(*mp) +		/* build get reply hdr size 	*/
			(count - 1) * 		/* size of n-1 array elements 	*/
				dbr_value_size[type] 	
			+ dbr_size[type];	/* size of the structure fetched */

		get_reply = (struct extmsg *) ALLOC_MSG(client, size);
		if (!get_reply) {
			/* tell them that their request is to large */
			send_err(mp, ECA_TOLARGE, client, RECORD_NAME(&tmp_addr));
		} else {
			struct event_ext evext;

			evext.mp = mp + 1;
			evext.pciu = pchannel;
			evext.mp->m_cid = sid;
			evext.send_lock = FALSE;
			evext.size = size;
			evext.get = TRUE;

			/*
			 * Arguments to this routine organized in favor of
			 * the standard	db event calling mechanism-
			 * routine(userarg, paddr). See events added above.
			 * Hold argument set true so the send message buffer
			 * is not flushed once each call.
			 */
			read_reply(&evext, &tmp_addr, TRUE, NULL);

			/*
			 * this allows extra build replies 
			 * to be dicarded
			 */
			get_reply->m_cmmd = IOC_READ_BUILD;
		}
	}
	search_reply = (struct extmsg *) ALLOC_MSG(client, 0);
	if (!search_reply)
		taskSuspend(0);

	*search_reply = *mp;
	search_reply->m_postsize = 0;

	/* this field for rmt machines where paddr invalid */
	search_reply->m_type = tmp_addr.field_type;
	search_reply->m_count = tmp_addr.no_elements;
	search_reply->m_cid = sid;

	END_MSG(client);
	UNLOCK_CLIENT(client);

	return;
}


/* 	search_fail_reply()
 *
 *	Only when requested by the client 
 *	send search failed reply
 *
 *
 */
LOCAL void search_fail_reply(
struct extmsg *mp,
struct client  *client
)
{
	FAST struct extmsg *reply;

	LOCK_CLIENT(client);
	reply = (struct extmsg *) ALLOC_MSG(client, 0);
	if (!reply) {
		taskSuspend(0);
	}
	*reply = *mp;
	reply->m_cmmd = IOC_NOT_FOUND;
	reply->m_postsize = 0;

	END_MSG(client);
	UNLOCK_CLIENT(client);

}


/* 	send_err()
 *
 *	reflect error msg back to the client
 *
 *	send buffer lock must be on while in this routine
 *
 */
LOCAL void send_err(
struct extmsg  *curp,
int            status,
struct client  *client,
char           *footnote
)
{
        struct channel_in_use *pciu;
	int        size;
	struct extmsg *reply;


	/*
	 * force string post size to be the true size rounded to even
	 * boundary
	 */
	size = strlen(footnote)+1;
	size += sizeof(*curp);

	reply = (struct extmsg *) ALLOC_MSG(client, size);
	if (!reply){
		printf(	"caserver: Unable to deliver err msg [%s]\n",
			ca_message(status));
		return;
	}

	*reply = nill_msg;
	reply->m_cmmd = IOC_ERROR;
	reply->m_available = status;
	reply->m_postsize = size;
	switch (curp->m_cmmd) {
	case IOC_EVENT_ADD:
	case IOC_EVENT_CANCEL:
	case IOC_READ:
	case IOC_READ_NOTIFY:
	case IOC_SEARCH:
	case IOC_BUILD:
	case IOC_WRITE:
	        /*
		 *
		 * Verify the channel
		 *
		 */
		pciu = MPTOPCIU(curp);
		if(pciu){
			reply->m_cid = (unsigned long) pciu->cid;
		}
		else{
			reply->m_cid = ~0L;
		}
		break;
	case IOC_EVENTS_ON:
	case IOC_EVENTS_OFF:
	case IOC_READ_SYNC:
	case IOC_SNAPSHOT:
	default:
		reply->m_cid = NULL;
		break;
	}
	*(reply + 1) = *curp;
	strcpy((char *)(reply + 2), footnote);

	END_MSG(client);

}


/*
 * logBadId()
 */
LOCAL void logBadId(
struct client *client, 
struct extmsg *mp
)
{
	send_err(mp, ECA_INTERNAL, client, "ID lookup failed");
}


/* 	log_header()
 *
 *	Debug aid - print the header part of a message.
 *
 */
LOCAL void log_header(
struct extmsg 	*mp,
int		mnum
)
{
        struct channel_in_use *pciu;

	pciu = MPTOPCIU(mp);

	logMsg(	"CAS: N=%d cmd=%d type=%d pstsize=%d paddr=%x avail=%x\n",
	  	mnum, 
	  	mp->m_cmmd,
	  	mp->m_type,
	  	mp->m_postsize,
	  	(int)(pciu?&pciu->addr:NULL),
	  	(int)mp->m_available);
  	if(mp->m_cmmd==IOC_WRITE && mp->m_type==DBF_STRING)
    		logMsg("CAS: The string written: %s \n",
			(int)(mp+1),
			NULL,
			NULL,
			NULL,
			NULL,
			NULL);
}



/*
 *
 *      cac_send_heartbeat()
 *
 *	lock must be applied while in this routine
 */
void cas_send_heartbeat(
struct client	*pc
)
{
        FAST struct extmsg 	*reply;

        reply = (struct extmsg *) ALLOC_MSG(pc, 0);
        if(!reply){
                taskSuspend(0);
	}

	*reply = nill_msg;
        reply->m_cmmd = IOC_NOOP;

        END_MSG(pc);

        return;
}



/*
 * MPTOPCIU()
 *
 * used to be a macro
 */
LOCAL struct channel_in_use *MPTOPCIU(struct extmsg *mp)
{
	struct channel_in_use *pciu;

	FASTLOCK(&rsrv_free_addrq_lck);
	pciu = bucketLookupItem(pCaBucket, mp->m_cid);
	FASTUNLOCK(&rsrv_free_addrq_lck);

	return pciu;
}

