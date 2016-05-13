/* 
** Tmsrv server - transaction monitor
** After that log transaction to hash & to disk for tracking the stuff...
** TODO: We should have similar control like "TP_COMMIT_CONTROL" -
** either return after stuff logged or after really commit completed.
** Error handling:
** - System errors we will track via ATMI interface error functions
** - XA errors will be tracked via XA error interface
**
** @file tmapi.c
** 
** -----------------------------------------------------------------------------
** Enduro/X Middleware Platform for Distributed Transaction Processing
** Copyright (C) 2015, ATR Baltic, SIA. All Rights Reserved.
** This software is released under one of the following licenses:
** GPL or ATR Baltic's license for commercial use.
** -----------------------------------------------------------------------------
** GPL license:
** 
** This program is free software; you can redistribute it and/or modify it under
** the terms of the GNU General Public License as published by the Free Software
** Foundation; either version 2 of the License, or (at your option) any later
** version.
**
** This program is distributed in the hope that it will be useful, but WITHOUT ANY
** WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
** PARTICULAR PURPOSE. See the GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License along with
** this program; if not, write to the Free Software Foundation, Inc., 59 Temple
** Place, Suite 330, Boston, MA 02111-1307 USA
**
** -----------------------------------------------------------------------------
** A commercial use license is available from ATR Baltic, SIA
** contact@atrbaltic.com
** -----------------------------------------------------------------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <regex.h>
#include <utlist.h>

#include <ndebug.h>
#include <atmi.h>
#include <atmi_int.h>
#include <typed_buf.h>
#include <ndrstandard.h>
#include <ubf.h>
#include <Exfields.h>

#include <exnet.h>
#include <ndrxdcmn.h>

#include "tmsrv.h"
#include "../libatmisrv/srv_int.h"
#include "tperror.h"
#include <xa_cmn.h>
/*---------------------------Externs------------------------------------*/
/*---------------------------Macros-------------------------------------*/
/*---------------------------Enums--------------------------------------*/
/*---------------------------Typedefs-----------------------------------*/
/*---------------------------Globals------------------------------------*/
/*---------------------------Statics------------------------------------*/
/*---------------------------Prototypes---------------------------------*/

/******************************************************************************/
/*                               TP API COMMANDS                              */
/******************************************************************************/

/**
 * TP API
 * TP entry for abort.
 * @param p_ub
 * @return 
 */
public int tm_tpabort(UBFH *p_ub)
{
    int ret = SUCCEED;
    atmi_xa_tx_info_t xai;
    atmi_xa_log_t *p_tl = NULL;
    int i;
    
    NDRX_LOG(log_debug, "tm_tpabort() called");
    
    /* 1. get transaction from hash */
    if (SUCCEED!=atmi_xa_read_tx_info(p_ub, &xai))
    {
        NDRX_LOG(log_error, "Failed to read transaction data");
        /* - will assume that completed OK!
        atmi_xa_set_error_msg(p_ub, TPESYSTEM, NDRX_XA_ERSN_INVPARAM, 
                "Invalid transaction data (missing fields)");
        */
        atmi_xa_set_error_fmt(p_ub, TPEINVAL, NDRX_XA_ERSN_NOTX, 
                "Transaction with xid [%s] not logged - probably was tout+abort", 
                xai.tmxid);
        ret=SUCCEED;
        goto out;
    }
    
    /* read tx from hash */
    if (NULL==(p_tl = tms_log_get_entry(xai.tmxid)))
    {
        NDRX_LOG(log_error, "Transaction with xid [%s] not logged", 
                xai.tmxid);
        atmi_xa_set_error_fmt(p_ub, TPEPROTO, NDRX_XA_ERSN_NOTX, 
                "Transaction with xid [%s] not logged", xai.tmxid);
        FAIL_OUT(ret);
    }
    
    /* Switch the state to aborting... */

    if (SUCCEED!=tms_log_stage(p_tl, XA_TX_STAGE_ABORTING))    
    {
        NDRX_LOG(log_error, "Failed to log ABORTING stage!");
        atmi_xa_set_error_fmt(p_ub, TPESYSTEM, NDRX_XA_ERSN_LOGFAIL, 
                "Cannot log [%s] tx!", xai.tmxid);
        FAIL_OUT(ret);
    }
    
    /* Call internal version of abort */
    if (SUCCEED!=(ret=tm_drive(&xai, p_tl, XA_OP_ROLLBACK, FAIL)))
    {
        atmi_xa_set_error_fmt(p_ub, ret, NDRX_XA_ERSN_RMCOMMITFAIL, 
                "Distributed transaction process did not finish completely");
        ret = SUCCEED;
    }
        
out:
        
    return ret;
}

/**
 * TP API
 * Commit the transaction (master entry)
 * Transaction initiator ask to commit the global transaction.
 * @param p_ub
 * @return 
 */
public int tm_tpcommit(UBFH *p_ub)
{
    int ret = SUCCEED;
    atmi_xa_tx_info_t xai;
    atmi_xa_log_t *p_tl = NULL;
    int i;
    int do_abort = FALSE;
    short reason;
    
    NDRX_LOG(log_debug, "tm_tpcommit() called");
    
    /* 1. get transaction from hash */
    if (SUCCEED!=atmi_xa_read_tx_info(p_ub, &xai))
    {
        NDRX_LOG(log_error, "Failed to read transaction data");
        atmi_xa_set_error_msg(p_ub, TPESYSTEM, NDRX_XA_ERSN_INVPARAM, 
                "Invalid transaction data (missing fields)");
        FAIL_OUT(ret);
    }
    
    /* read tx from hash */
    if (NULL==(p_tl = tms_log_get_entry(xai.tmxid)))
    {
        NDRX_LOG(log_error, "Transaction with xid [%s] not logged", 
                xai.tmxid);
        /* - Will regsiter as rolled back!!!
        atmi_xa_set_error_fmt(p_ub, TPEPROTO, NDRX_XA_ERSN_NOTX, 
                "Transaction with xid [%s] not logged", xai.tmxid);
         */
        atmi_xa_set_error_fmt(p_ub, TPEABORT, NDRX_XA_ERSN_NOTX, 
                "Transaction with xid [%s] not logged - probably was tout+abort", xai.tmxid);
        FAIL_OUT(ret);
    }
    
    /* Open log file */
    if (SUCCEED!=tms_open_logfile(p_tl, "w"))
    {
        NDRX_LOG(log_error, "Failed to open log file");
        atmi_xa_set_error_msg(p_ub, TPESYSTEM, NDRX_XA_ERSN_INVPARAM, 
                "Failed to open log file");
        do_abort = TRUE;
        FAIL_OUT(ret);
    }
    
    /* Log that we start commit... */
    if (SUCCEED!=tms_log_info(p_tl) ||
            SUCCEED!=tms_log_stage(p_tl, XA_TX_STAGE_PREPARING))    
    {
        NDRX_LOG(log_error, "Failed to log tx - abort!");
        do_abort = TRUE;
        FAIL_OUT(ret);
    }
    
    /* Drive - it will auto-rollback if needed... */
    if (SUCCEED!=(ret=tm_drive(&xai, p_tl, XA_OP_COMMIT, FAIL)))
    {
        atmi_xa_set_error_msg(p_ub, ret, NDRX_XA_ERSN_RMCOMMITFAIL, 
                "Transaction did not complete fully");
        ret=FAIL;
        goto out;
    }
    
out:
                
    /* Rollback the work done! */
    if (do_abort)
    {
        NDRX_LOG(log_warn, "About to rollback transaction!");
        
        tms_log_stage(p_tl, XA_TX_STAGE_ABORTING);

        /* Call internal version of abort */
        if (SUCCEED!=(ret=tm_drive(&xai, p_tl, XA_OP_COMMIT, FAIL)))
        {
            atmi_xa_override_error(p_ub, ret);
            ret=FAIL;
        }
    }

    return ret;
}

/**
 * TP API
 * Start new XA transaction
 * @param p_ub
 * @return 
 */
public int tm_tpbegin(UBFH *p_ub)
{
    int ret=SUCCEED;
    XID xid; /* handler for new XID */
    atmi_xa_tx_info_t xai;
    int do_rollback=FALSE;
    unsigned char xid_str[NDRX_XID_SERIAL_BUFSIZE];
    long txtout;
    long tmflags;
    NDRX_LOG(log_debug, "tm_tpbegin() called");
    
    if (SUCCEED!=Bget(p_ub, TMTXFLAGS, 0, (char *)&tmflags, 0L))
    {
        NDRX_LOG(log_error, "Failed to read TMTXFLAGS!");
        FAIL_OUT(ret);   
    }
    
    atmi_xa_new_xid(&xid);
    
    /* we should start new transaction... (only if static...) */
    if (!(tmflags & TMTXFLAGS_DYNAMIC_REG))
    {
        if (SUCCEED!=(ret = atmi_xa_start_entry(&xid, 0)))
        {
            NDRX_LOG(log_error, "Failed to start new transaction!");
            atmi_xa_set_error_fmt(p_ub, TPETRAN, NDRX_XA_ERSN_NONE, 
                    "Failed to start new transaction, "
                    "xa error: %d [%s]", ret, atmi_xa_geterrstr(ret));
            goto out;
        }
        
        xai.tmknownrms[0] = G_atmi_env.xa_rmid;
        xai.tmknownrms[1] = EOS;
    }
    else
    {
        xai.tmknownrms[0] = EOS;
    }
    
    atmi_xa_serialize_xid(&xid, xid_str);
    
    /* load the XID into buffer */
    strcpy(xai.tmxid, xid_str);
    xai.tmrmid = G_atmi_env.xa_rmid;
    xai.tmnodeid = G_atmi_env.our_nodeid;
    xai.tmsrvid = G_server_conf.srv_id;
    
    
    /* Currently time-out is handled only locally by TM */
    if (SUCCEED!=Bget(p_ub, TMTXTOUT, 0, (char *)&txtout, 0L) || 0>=txtout)
    {
        txtout = G_tmsrv_cfg.dflt_timeout;
        NDRX_LOG(log_debug, "TX tout defaulted to: %ld", txtout);
    }
    else
    {
        NDRX_LOG(log_debug, "TX tout: %ld", txtout);
    }
    /* Only for static... */
    if (!(tmflags & TMTXFLAGS_DYNAMIC_REG))
    {
        if (SUCCEED!=(ret = atmi_xa_end_entry(&xid)))
        {
            NDRX_LOG(log_error, "Failed to end XA api!");
            atmi_xa_set_error_fmt(p_ub, TPETRAN, NDRX_XA_ERSN_NONE, 
                    "Failed to start end transaction, "
                    "xa error: %d [%s]", ret, atmi_xa_geterrstr(ret));
            goto out;
        }
    }
    
    if (SUCCEED!=atmi_xa_load_tx_info(p_ub, &xai))
    {
        NDRX_LOG(log_error, "Failed to load tx info!");
        atmi_xa_set_error_fmt(p_ub, TPETRAN, NDRX_XA_ERSN_NONE, 
                    "Failed to return tx info!");
        do_rollback = TRUE;
        ret=FAIL;
        goto out;
    }
    
    /* Log into journal */
    if (SUCCEED!=tms_log_start(&xai, txtout, tmflags))
    {
        NDRX_LOG(log_error, "Failed to log the transaction!");
        atmi_xa_set_error_fmt(p_ub, TPETRAN, NDRX_XA_ERSN_LOGFAIL, 
                    "Failed to log the transaction!");
        do_rollback = TRUE;
        ret=FAIL;
        goto out;
    }
    
out:
    
    /* We should abort the transaction right now */
    if (do_rollback)
    {
        ret = tm_rollback_local(p_ub, &xai);
    }

    return ret;
}

/******************************************************************************/
/*                         TRANSACTION BRANCH COMMANDS                        */
/******************************************************************************/

/**
 * Register resource manager under given transaction
 * @param p_ub
 * @return 
 */
public int tm_tmregister(UBFH *p_ub)
{
    int ret = SUCCEED;
    short   callerrm;
    int is_already_logged = FALSE;
    atmi_xa_tx_info_t xai;
    long tmflags = 0;
    
    if (SUCCEED!=Bget(p_ub, TMCALLERRM, 0, (char *)&callerrm, 0L))
    {
        atmi_xa_set_error_fmt(p_ub, TPESYSTEM, NDRX_XA_ERSN_INVPARAM, 
                    "Missing TMCALLERRM field: %s!", Bstrerror(Berror));
        FAIL_OUT(ret);
    }
    
    if (SUCCEED!=atmi_xa_read_tx_info(p_ub, &xai))
    {
        atmi_xa_set_error_fmt(p_ub, TPESYSTEM, NDRX_XA_ERSN_INVPARAM, 
                    "Failed to read transaction info!");
        FAIL_OUT(ret);
    }
    
    if (SUCCEED!=tms_log_addrm(&xai, callerrm, &is_already_logged))
    {
        atmi_xa_set_error_fmt(p_ub, TPESYSTEM, NDRX_XA_ERSN_RMLOGFAIL, 
                    "Failed to log new RM!");
        FAIL_OUT(ret);
    }
    
    if (is_already_logged)
    {
        tmflags|=TMTXFLAGS_RMIDKNOWN;
    }
    
    if (SUCCEED!=Bchg(p_ub, TMTXFLAGS, 0, (char *)&tmflags, 0L))
    {
        atmi_xa_set_error_fmt(p_ub, TPESYSTEM, NDRX_XA_ERSN_UBFERR, 
                    "Failed to set TMTXFLAGS!");
        FAIL_OUT(ret);
    }
    
out:
    return ret; 
}


/**
 * Do the internal prepare of transaction (request sent from other TM)
 * @param p_ub
 * @return 
 */
public int tm_tmprepare(UBFH *p_ub)
{
    int ret = SUCCEED;
    atmi_xa_tx_info_t xai;
    
    NDRX_LOG(log_debug, "tm_tmprepare called.");
    /* read xai from FB... */
    if (SUCCEED!=atmi_xa_read_tx_info(p_ub, &xai))
    {
        atmi_xa_set_error_fmt(p_ub, TPESYSTEM, NDRX_XA_ERSN_INVPARAM, 
                    "Failed to read transaction info!");
        FAIL_OUT(ret);
    }
    
    if (SUCCEED!=(ret = tm_prepare_local(p_ub, &xai)))
    {
        FAIL_OUT(ret);
    }
    
out:
    return ret;
}

/**
 * Do the internal commit of transaction (request sent from other TM)
 * @param p_ub
 * @return 
 */
public int tm_tmcommit(UBFH *p_ub)
{
    int ret = SUCCEED;
    atmi_xa_tx_info_t xai;
    
    NDRX_LOG(log_debug, "tm_tmcommit called.");
    /* read xai from FB... */
    if (SUCCEED!=atmi_xa_read_tx_info(p_ub, &xai))
    {
        atmi_xa_set_error_fmt(p_ub, TPESYSTEM, NDRX_XA_ERSN_INVPARAM, 
                    "Failed to read transaction info!");
        FAIL_OUT(ret);
    }
    
    if (SUCCEED!=(ret = tm_commit_local(p_ub, &xai)))
    {
        FAIL_OUT(ret);
    }
    
out:
    return ret;
}

/**
 * Local transaction branch abort command. Sent from Master TM
 * @param p_ub
 * @return 
 */
public int tm_tmabort(UBFH *p_ub)
{
    int ret = SUCCEED;
    atmi_xa_tx_info_t xai;
    
    NDRX_LOG(log_debug, "tm_tmabort called.");
    if (SUCCEED!=atmi_xa_read_tx_info(p_ub, &xai))
    {
        atmi_xa_set_error_fmt(p_ub, TPESYSTEM, NDRX_XA_ERSN_INVPARAM, 
                    "Failed to read transaction info!");
        FAIL_OUT(ret);
    }
    
    /* read xai from FB... */
    if (SUCCEED!=(ret = tm_rollback_local(p_ub, &xai)))
    {
        FAIL_OUT(ret);
    }
    
out:
    return ret;
}

/******************************************************************************/
/*                         COMMAND LINE API                                   */
/******************************************************************************/
/**
 * Return list of transactions
 * @param p_ub
 * @param cd - call descriptor
 * @return 
 */
public int tm_tpprinttrans(UBFH *p_ub, int cd)
{
    int ret = SUCCEED;
    long revent;
    atmi_xa_log_list_t *tx_list;
    atmi_xa_log_list_t *el, *tmp;
    
    /* Get the lock! */
    tms_tx_hash_lock();
    
    tx_list = tms_copy_hash2list(COPY_MODE_FOREGROUND | COPY_MODE_BACKGROUND);
        
    LL_FOREACH_SAFE(tx_list,el,tmp)
    {
        
        /* Erase FB & setup the info there... */
        Bproj(p_ub, NULL); /* clear the FB! */
        if (SUCCEED!=tms_log_cpy_info_to_fb(p_ub, &(el->p_tl)))
        {
            FAIL_OUT(ret);
        }
        
        
        /* Bfprint(p_ub, stderr); */
        
        if (FAIL == tpsend(cd,
                            (char *)p_ub,
                            0L,
                            0,
                            &revent))
        {
            NDRX_LOG(log_error, "Send data failed [%s] %ld",
                                tpstrerror(tperrno), revent);
            FAIL_OUT(ret);
        }
        else
        {
            NDRX_LOG(log_debug,"sent ok");
        }
        
        LL_DELETE(tx_list, el);
        free(el);
    }
    
out:
    /* TODO: might want to kill the list if FAIL!!!! */
    tms_tx_hash_unlock();

    return ret;
}

/**
 * Abort transaction.
 * @param p_ub
 * @param cd - call descriptor
 * @return 
 */
public int tm_aborttrans(UBFH *p_ub)
{
    int ret = SUCCEED;
    atmi_xa_log_t *p_tl;
    char tmxid[NDRX_XID_SERIAL_BUFSIZE+1];
    short tmrmid = FAIL;
    atmi_xa_tx_info_t xai;
    /* We should try to abort transaction
     Thus basically we need to lock the transaction on which we work on.
     Otherwise, we can conflict with background.
     */
    background_lock();
    
    if (SUCCEED!=Bget(p_ub, TMXID, 0, tmxid, 0L))
    {
        NDRX_LOG(log_error, "Failed to read TMXID: %s", 
                Bstrerror(Berror));
        atmi_xa_set_error_msg(p_ub, TPESYSTEM, 0, "Protocol error, missing TMXID!");
        FAIL_OUT(ret);
    }
    
    /* optional */
    Bget(p_ub, TMTXRMID, 0, (char *)&tmrmid, 0L);
    
    /* Lookup for log. And then try to abort... */
    if (NULL==(p_tl = tms_log_get_entry(tmxid)))
    {
        /* Generate error */
        atmi_xa_set_error_fmt(p_ub, TPEMATCH, 0, "Transaction not found (%s)!", 
                tmxid);
        FAIL_OUT(ret);
    }
    
    /* Try to abort stuff... */
    
    /* init xai for tl... */
    XA_TX_COPY((&xai), p_tl);
    
    NDRX_LOG(log_debug, "Got RMID: [%hd]", tmrmid);
    
    /* Switch transaction to aborting (if not already) */
    tms_log_stage(p_tl, XA_TX_STAGE_ABORTING);
    if (SUCCEED!=(ret=tm_drive(&xai, p_tl, XA_OP_ROLLBACK, tmrmid)))
    {
        atmi_xa_set_error_fmt(p_ub, ret, NDRX_XA_ERSN_RMERR, 
                "Failed to abort transaction");
        ret=FAIL;
        goto out;
    }
    
out:
    background_unlock();

    return ret;
}



/**
 * Commit transaction.
 * @param p_ub
 * @param cd - call descriptor
 * @return 
 */
public int tm_committrans(UBFH *p_ub)
{
    int ret = SUCCEED;
    atmi_xa_log_t *p_tl;
    char tmxid[NDRX_XID_SERIAL_BUFSIZE+1];
    atmi_xa_tx_info_t xai;
    /* We should try to commit transaction
     Thus basically we need to lock the transaction on which we work on.
     Otherwise, we can conflict with background.
     */
    background_lock();
    
    if (SUCCEED!=Bget(p_ub, TMXID, 0, tmxid, 0L))
    {
        NDRX_LOG(log_error, "Failed to read TMXID: %s", 
                Bstrerror(Berror));
        atmi_xa_set_error_msg(p_ub, TPESYSTEM, 0, "Protocol error, missing TMXID!");
        FAIL_OUT(ret);
    }
    
    /* Lookup for log. And then try to commit... */
    if (NULL==(p_tl = tms_log_get_entry(tmxid)))
    {
        /* Generate error */
        atmi_xa_set_error_fmt(p_ub, TPEMATCH, 0, "Transaction not found (%s)!", 
                tmxid);
        FAIL_OUT(ret);
    }
    
    /* Try to commit stuff, only if stage is prepared...! */
    if (XA_TX_STAGE_COMMITTING!=p_tl->txstage)
    {
        atmi_xa_set_error_fmt(p_ub, TPEINVAL, 0, 
                "Transaction not in PREPARED stage!");
        FAIL_OUT(ret);
    }
    
    /* init xai for tl... */
    XA_TX_COPY((&xai), p_tl);
    
    if (SUCCEED!=(ret=tm_drive(&xai, p_tl, XA_OP_COMMIT, FAIL)))
    {
        atmi_xa_set_error_fmt(p_ub, ret, NDRX_XA_ERSN_RMCOMMITFAIL, 
                "Failed to commit transaction!");
        ret=FAIL;
        goto out;
    }
    
out:
    background_unlock();

    return ret;
}
