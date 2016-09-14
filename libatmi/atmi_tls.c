/* 
** Globals/TLS for libatmi
**
** @file atmi_tls.c
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

/*---------------------------Includes-----------------------------------*/
#include <stdlib.h>
#include <stdio.h>
#include <ndrstandard.h>
#include <atmi.h>
#include <atmi_tls.h>
#include <string.h>
#include "thlock.h"
#include "userlog.h"
/*---------------------------Externs------------------------------------*/
/*---------------------------Macros-------------------------------------*/
/*---------------------------Enums--------------------------------------*/
/*---------------------------Typedefs-----------------------------------*/
/*---------------------------Globals------------------------------------*/
__thread atmi_tls_t *G_atmi_tls = NULL; /* single place for library TLS storage */
/*---------------------------Statics------------------------------------*/
private pthread_key_t atmi_buffer_key;
MUTEX_LOCKDECL(M_thdata_init);
private int M_first = TRUE;
/*---------------------------Prototypes---------------------------------*/
private void atmi_buffer_key_destruct( void *value );

/**
 * Thread destructor
 * @param value this is malloced thread TLS
 */
private void atmi_buffer_key_destruct( void *value )
{
    ndrx_atmi_tls_free((void *)value);
    pthread_setspecific( atmi_buffer_key, NULL );
}

/**
 * Unlock, unset G_atmi_tls, return pointer to G_atmi_tls
 * @return 
 */
public void * ndrx_atmi_tls_get(void)
{
    atmi_tls_t *tmp = G_atmi_tls;
    
    G_atmi_tls = NULL;
    
    /* suspend the transaction if any in progress: similar to tpsrvgetctxdata() */
    if (tmp->G_atmi_xa_curtx.txinfo)
    {
        
        if (SUCCEED!=tpsuspend(&tmp->tranid, 0))
        {
            userlog("ndrx_atmi_tls_get: Failed to suspend transaction: [%s]", 
                    tpstrerror(tperrno));

            MUTEX_UNLOCK_V(tmp->mutex);
            
            ndrx_atmi_tls_free(tmp);
            /* fail it. */
            tmp = NULL;
            goto out;
        }
    }
    
    /* unlock object */
    MUTEX_UNLOCK_V(tmp->mutex);
    
out:
    return (void *)tmp;
}

/**
 * Get the lock & set the G_atmi_tls to this one
 * @param tls
 */
public int ndrx_atmi_tls_set(void *data, int flags)
{
    int ret = SUCCEED;
    atmi_tls_t *tls = (atmi_tls_t *)data;
    /* extra control... */
    if (ATMI_TLS_MAGIG!=tls->magic)
    {
        userlog("atmi_tls_set: invalid magic! expected: %x got %x", 
                ATMI_TLS_MAGIG, tls->magic);
    }

    /* Lock the object */
    MUTEX_LOCK_V(tls->mutex);

    
    /* Add the additional flags to the user. */
    tls->G_last_call.sysflags |= flags;

    if (tls->G_atmi_xa_curtx.txinfo && SUCCEED!=tpresume(&tls->tranid, 0))
    {
        userlog("Failed to resume transaction: [%s]", tpstrerror(tperrno));
    }
    
    G_atmi_tls = tls;
    
out:
    return ret;
}

/**
 * Free up the TLS data
 * @param tls
 * @return 
 */
public void ndrx_atmi_tls_free(void *data)
{
    free((char*)data);
}

/**
 * Get the lock & init the data
 * @param auto_destroy if set to 1 then when tried exits, thread data will be made free
 * @return 
 */
public void * ndrx_atmi_tls_new(int auto_destroy, int auto_set)
{
    int ret = SUCCEED;
    atmi_tls_t *tls  = NULL;
    char fn[] = "atmi_context_new";
    
    /* init they key storage */
    if (M_first)
    {
        MUTEX_LOCK_V(M_thdata_init);
        if (M_first)
        {
            pthread_key_create( &atmi_buffer_key, 
                    &atmi_buffer_key_destruct );
            M_first = FALSE;
        }
        MUTEX_UNLOCK_V(M_thdata_init);
    }
    
    if (NULL==(tls = (atmi_tls_t *)malloc(sizeof(atmi_tls_t))))
    {
        userlog ("%s: failed to malloc", fn);
        FAIL_OUT(ret);
    }
    
    /* do the common init... */
    tls->magic = ATMI_TLS_MAGIG;
    
    
    /* init.c */    
    tls->conv_cd=1;/*  first available */
    tls->callseq = 0;
    tls->G_atmi_is_init= 0;/*  Is environment initialised */
    
    /* tpcall.c */
    tls->M_svc_return_code = 0;
    tls->tpcall_first = TRUE;
    tls->tpcall_cd = 0;
    
    /* tperror.c */
    tls->M_atmi_error_msg_buf[0] = EOS;
    tls->M_atmi_error = TPMINVAL;
    tls->M_atmi_reason = NDRX_XA_ERSN_NONE;
    tls->errbuf[0] = EOS;
    /* xa.c */
    tls->M_is_curtx_init = FALSE;
    
    pthread_mutex_init(&tls->mutex, NULL);
    
    /* set callback, when thread dies, we need to get the destructor 
     * to be called
     */
    if (auto_destroy)
    {
        pthread_setspecific( atmi_buffer_key, (void *)tls );
    }
    
    if (auto_set)
    {
        ndrx_atmi_tls_set(tls, 0);
    }
    
out:

    if (SUCCEED!=ret && NULL!=tls)
    {
        ndrx_atmi_tls_free((char *)tls);
    }

    return (void *)tls;
}

