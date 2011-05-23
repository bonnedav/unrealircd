/************************************************************************
 *   IRC - Internet Relay Chat, s_unreal.c
 *   (C) 1999 Carsten Munk (Techie/Stskeeps) <cmunk@toybox.flirt.org>
 *
 *   See file AUTHORS in IRC package for additional names of
 *   the programmers. 
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "struct.h"
#include "common.h"
#include "sys.h"
#include "numeric.h"
#include "msg.h"
#include "channel.h"
#include "userload.h"
#include "version.h"
#include <time.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <utmp.h>
#else
#include <io.h>
#endif
#include <fcntl.h>
#include "h.h"

ID_CVS("$Id$");
ID_Copyright("(C) Carsten Munk 1999");

time_t TSoffset = 0;

#ifndef NO_FDLIST
extern float currentrate;
extern float currentrate2;
extern float highest_rate;
extern float highest_rate2;
extern int   lifesux;
#endif
/*
   m_sethost() added by Stskeeps (30/04/1999)
               (modified at 15/05/1999) by Stskeeps | Potvin
   :prefix SETHOST newhost
   parv[0] - sender
   parv[1] - newhost
   D: this performs a mode +x function to set hostname
      to whatever you want to (if you are IRCop) **efg**
      Very experimental currently
   A: Remember to see server_etabl ;)))))
      *evil fucking grin*
*/
int     m_sethost(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int parc;
char *parv[];
{
	char	*vhost, *s;
	int	permit = 0; // 0 = opers(glob/locop) 1 = global oper 2 = not MY clients.. 
	int	donotice = 0; /* send out notices if local connect ( 0 = NOT 1 = yes ) */
	int	legalhost = 1; /* is legal characters? */

	if (check_registered(sptr))
        	return 0;

	if (!MyConnect(sptr))
		goto have_permit1;
     	switch (permit) {
		case 0: if (!IsAnOper(sptr)) {
					sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
					return 0;
			}
			break;
		case 1: if (!IsOper(sptr)) {
				sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
				return 0;
			}
			break;
		case 2: if (MyConnect(sptr)) {
				sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
				return 0;
			}
		default:
			sendto_ops_butone(IsServer(cptr) ? cptr : NULL, sptr,
			       ":%s WALLOPS :[SETHOST] Somebody fixing this corrupted server? !(0|1) !!!", me.name); 
				break;				                                
	}

	have_permit1:	
	if (parc < 2)
		vhost = NULL;
	else
		vhost = parv[1];

	/* bad bad bad boys .. ;p */	
	if (vhost == NULL) {
		if (MyConnect(sptr)) {
			sendto_one(sptr, ":%s NOTICE %s :*** Syntax: /SetHost <new host>", me.name, parv[0]);		
		}
		return;
	}
	/* uh uh .. too small */
	if (strlen(parv[1]) < 1) {
		if (MyConnect(sptr))
			sendto_one(sptr, ":%s NOTICE %s :*** /SetHost Error: Atleast write SOMETHING that makes sense (':' string)",
				me.name, sptr->name);
	}
	/* too large huh? */
	if (strlen(parv[1]) > (HOSTLEN - 1) ) {
		/* ignore us as well if we're not a child of 3k */
		if (MyConnect(sptr))
			sendto_one(sptr, ":%s NOTICE %s :*** /SetHost Error: Hostnames are limited to %i characters.", me.name, sptr->name, HOSTLEN);
		return;
	}
	
	/* illegal?! */
	for (s = vhost; *s; s++) {
		if (!isallowed(*s)) {
			legalhost = 0;
		}
	}
	
	if (legalhost == 0) {
		sendto_one(sptr, ":%s NOTICE %s :*** /SetHost Error: A hostname may contain a-z, A-Z, 0-9, '-' & '.' - Please only use them", me.name, parv[0]);
		return 0;
	}

    /* hide it */
    sptr->umodes |= UMODE_HIDE;
    sptr->umodes |= UMODE_SETHOST;
    /* get it in */
    sprintf(sptr->user->virthost, "%s", vhost);
    /* spread it out */
    sendto_serv_butone(cptr, ":%s SETHOST %s", sptr->name, parv[1]);
    
    if (MyConnect(sptr)) {
    	sendto_one(sptr, ":%s MODE %s :+xt", sptr->name, sptr->name);
    	sendto_one(sptr, ":%s NOTICE %s :Your nick!user@host-mask is now (%s!%s@%s) - To disable it type /mode %s -x", me.name, parv[0], 
                                  	parv[0], sptr->user->username, vhost, parv[0]);
    }
	return 0;
}

/* 
 * m_chghost - 12/07/1999 (two months after I made SETIDENT) - Stskeeps
 * :prefix CHGHOST <nick> <new hostname>
 * parv[0] - sender
 * parv[1] - nickname
 * parv[2] - hostname
 *
*/

int     m_chghost(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int parc;
char *parv[];
{
	aClient	*acptr;
	char	*s;
	int		legalhost = 1;

    if (check_registered(sptr))
         return 0;

         
	if (MyClient(sptr))
		if (!IsAnOper(sptr))
		{
				sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
			return 0;

		}

	if (parc < 3) {
		sendto_one(sptr, ":%s NOTICE %s :*** /ChgHost syntax is /ChgHost <nick> <newhost>", me.name, sptr->name);
		return 0;
	}
	
	if (strlen(parv[2]) < 1) {
		sendto_one(sptr, ":%s NOTICE %s :*** Write atleast something to change the host to!", me.name, sptr->name);
		return 0;
	}
	
	if (strlen(parv[2]) > (HOSTLEN - 1)) {
		sendto_one(sptr, ":%s NOTICE %s :*** ChgHost Error: Too long hostname!!", me.name, sptr->name);
	}

	/* illegal?! */
	for (s = parv[2]; *s; s++) {
		if (!isallowed(*s)) {
			legalhost = 0;
		}
	}
	
	if (legalhost == 0) {
		sendto_one(sptr, ":%s NOTICE %s :*** /ChgHost Error: A hostname may contain a-z, A-Z, 0-9, '-' & '.' - Please only use them", me.name, parv[0]);
		return 0;
	}

	if ((acptr = find_person(parv[1], NULL))) {
		if (!IsULine(cptr, sptr)) {
			sendto_umode(UMODE_EYES, "%s changed the virtual hostname of %s (%s@%s) to be %s",
						sptr->name, acptr->name, acptr->user->username,
						(acptr->umodes & UMODE_HIDE ? acptr->user->virthost : acptr->user->realhost),
						parv[2]);
		}
		acptr->umodes |= UMODE_HIDE;		  		
		acptr->umodes |= UMODE_SETHOST;
		sendto_serv_butone(cptr, ":%s CHGHOST %s %s",
  					sptr->name,
  					acptr->name,
  					parv[2]);
  		sprintf(acptr->user->virthost, "%s", parv[2]);
		return 0;
	}
	 else
	{
		sendto_one(sptr, err_str(ERR_NOSUCHNICK), me.name, sptr->name, parv[1]);
		return 0;
	}
	return 0;
}

/* 
 * m_chgident - 12/07/1999 (two months after I made SETIDENT) - Stskeeps
 * :prefix CHGHOST <nick> <new identname>
 * parv[0] - sender
 * parv[1] - nickname
 * parv[2] - identname
 *
*/

int     m_chgident(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int parc;
char *parv[];
{
	aClient	*acptr;
	char	*s;
	int		legalident = 1;

    if (check_registered(sptr))
         return 0;

         
	if (MyClient(sptr))
		if (!IsAnOper(sptr))
		{
				sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
			return 0;

		}

	if (parc < 3) {
		sendto_one(sptr, ":%s NOTICE %s :*** /ChgIdent syntax is /ChgIdent <nick> <newident>", me.name, sptr->name);
		return 0;
	}
	
	if (strlen(parv[2]) < 1) {
		sendto_one(sptr, ":%s NOTICE %s :*** Write atleast something to change the ident to!", me.name, sptr->name);
		return 0;
	}
	
	if (strlen(parv[2]) > (HOSTLEN - 1)) {
		sendto_one(sptr, ":%s NOTICE %s :*** ChgIdent Error: Too long ident!!", me.name, sptr->name);
	}

	/* illegal?! */
	for (s = parv[2]; *s; s++) {
		if (!isallowed(*s)) {
			legalident = 0;
		}
	}
	
	if (legalident == 0) {
		sendto_one(sptr, ":%s NOTICE %s :*** /ChgIdent Error: A ident may contain a-z, A-Z, 0-9, '-' & '.' - Please only use them", me.name, parv[0]);
		return 0;
	}

	if ((acptr = find_person(parv[1], NULL))) {
		if (!IsULine(cptr, sptr)) {
			sendto_umode(UMODE_EYES, "%s changed the virtual ident of %s (%s@%s) to be %s",
						sptr->name, acptr->name, acptr->user->username,
						(acptr->umodes & UMODE_HIDE ? acptr->user->realhost : acptr->user->realhost),
						parv[2]);
		}
		sendto_serv_butone(cptr, ":%s CHGIDENT %s %s",
  					sptr->name,
  					acptr->name,
  					parv[2]);
  		sprintf(acptr->user->username, "%s", parv[2]);
		return 0;
	}
	 else
	{
		sendto_one(sptr, err_str(ERR_NOSUCHNICK), me.name, sptr->name, parv[1]);
		return 0;
	}
	return 0;
}

/* m_setident - 12/05/1999 - Stskeeps
 *  :prefix SETIDENT newident
 *  parv[0] - sender
 *  parv[1] - newident
 *  D: This will set your username to be <x> (like (/setident Root))
 *     (if you are IRCop) **efg*
 *     Very experimental currently
 * 	   Cloning of m_sethost at some points - so same authors ;P
*/

int     m_setident(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int parc;
char *parv[];
{

	char	*vident, *s;
	int		permit = 0; /* 0 = opers(glob/locop) 1 = global oper */
	int		donotice = 0; /* send out notices if local connect ( 0 = NOT 1 = yes )*/
	int		legalident = 1; /* is legal characters? */
    if (check_registered(sptr))
         return 0;
	if (!MyConnect(sptr))
		goto permit_2;
	switch (permit) {
		case 0: if (!IsAnOper(sptr)) {
					sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
					return 0;
				}
				break;
		case 1: if (!IsOper(sptr)) {
					sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
					return 0;
				}
				break;
		case 2: if (MyConnect(sptr)) {
					sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
					return 0;					
				}
				break;
		default:
				sendto_ops_butone(IsServer(cptr) ? cptr : NULL, sptr,
				       ":%s WALLOPS :[SETIDENT] Somebody fixing this corrupted server? !(0|1) !!!", me.name); 
				break;				                                
	}
	permit_2:	
	if (parc < 2)
		vident = NULL;
	else
		vident = parv[1];

	/* bad bad bad boys .. ;p */	
	if (vident == NULL) {
		if (MyConnect(sptr)) {
			sendto_one(sptr, ":%s NOTICE %s :*** Syntax: /SetIdent <new host>", me.name, parv[0]);		
		}
		return;
	}
	if (strlen(parv[1]) < 1) {
		if (MyConnect(sptr))
			sendto_one(sptr, ":%s NOTICE %s :*** /SetIdent Error: Atleast write SOMETHING that makes sense (':' string)",
				me.name, sptr->name);
	}

	/* too large huh? */
	if (strlen(vident) > ( USERLEN - 1)) {
		/* ignore us as well if we're not a child of 3k */
		if (MyConnect(sptr))
			sendto_one(sptr, ":%s NOTICE %s :*** /SetIdent Error: Usernames are limited to %i characters.", me.name, sptr->name, USERLEN);
		return;
	}
	
	/* illegal?! */
	for (s = vident; *s; s++) {
		if (!isallowed(*s)) {
			legalident = 0;
		}
		if (*s == '~')
			legalident = 1;	

	}
	
	if (legalident == 0) {
		sendto_one(sptr, ":%s NOTICE %s :*** /SetIdent Error: A username may contain a-z, A-Z, 0-9, '-', '~' & '.' - Please only use them", me.name, parv[0]);
		return 0;
	}
   
    /* get it in */
    sprintf(sptr->user->username, "%s", vident);
    /* spread it out */
    sendto_serv_butone(cptr, ":%s SETIDENT %s", sptr->name, parv[1]);
    
    if (MyConnect(sptr)) {
    	sendto_one(sptr, ":%s NOTICE %s :Your nick!user@host-mask is now (%s!%s@%s) - To disable ident set change it manually by /setident'ing again", me.name, parv[0], 
                                  	parv[0], sptr->user->username, 
                                  	IsHidden(sptr) ? sptr->user->virthost : sptr->user->realhost);
	}
    return;
}
/* m_setname - 12/05/1999 - Stskeeps
 *  :prefix SETNAME :gecos
 *  parv[0] - sender
 *  parv[1] - gecos
 *  D: This will set your gecos to be <x> (like (/setname :The lonely wanderer))
   yes it is experimental but anyways ;P
    FREEDOM TO THE USERS! ;)
*/

int     m_setname(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int parc;
char *parv[];
{
	if (parc < 2)
		return;
	if (strlen(parv[1]) > (REALLEN - 2)) {
		if (MyConnect(sptr)) {
			sendto_one(sptr, ":%s NOTICE %s :*** /SetName Error: \"Real names\" may maximum be %i characters of length", me.name, sptr->name, REALLEN);
		}
		return 0;
	}
	
	if (strlen(parv[1]) < 1) {
		sendto_one(sptr, ":%s NOTICE %s :Couldn't change realname - Nothing in parameter",
			me.name, sptr->name);
		return 0;
	}
		else 
			sprintf(sptr->info, "%s", parv[1]);
	

    sendto_serv_butone(&me, ":%s SETNAME :%s", parv[0], parv[1]);
	if (MyConnect(sptr))
       		 sendto_one(sptr, ":%s NOTICE %s :Your \"real name\" is now set to be %s - you have to set it manually to undo it", me.name, parv[0], parv[1]);      
          return 0;
 
//	sendto_serv_butone(cptr, ":%s SETNAME %s", parv[0], parv[1]);
	return 0;
}

/* m_sdesc - 15/05/1999 - Stskeeps
 *  :prefix SDESC
 *  parv[0] - sender
 *  parv[1] - description
 *  D: Sets server info if you are Server Admin (ONLINE)
*/

int     m_sdesc(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int parc;
char *parv[];
{
	if (IsCoAdmin(sptr)) 
		goto sdescok;
	/* ignoring */
	if (!IsAdmin(sptr)) 
		return;
sdescok:

	if (parc < 2)
		return;
	if (strlen(parv[1]) < 1) {
		if (MyConnect(sptr)) {
			sendto_one(sptr, ":%s NOTICE %s :*** Nothing to change to (SDESC)", me.name, sptr->name);
		}		
	}
	if (strlen(parv[1]) > (REALLEN - 1)) {
		if (MyConnect(sptr)) {
			sendto_one(sptr, ":%s NOTICE %s :*** /SDESC Error: \"Server info\" may maximum be %i characters of length", me.name, sptr->name, REALLEN);
		}
		return 0;
	}

	sprintf(sptr->srvptr->info, "%s", parv[1]);
	
    sendto_serv_butone(&me, ":%s SDESC :%s", parv[0], parv[1]);
	
	if (MyConnect(sptr))
        sendto_one(sptr, ":%s NOTICE %s :Your \"server description\" is now set to be %s - you have to set it manually to undo it", me.name, parv[0], parv[1]);      
        return 0;

	sendto_ops("Server description for %s is now '%s' changed by %s", sptr->srvptr->name, sptr->srvptr->info, parv[0]);
}


/*
** m_admins (Admin chat only) -Potvin
**      parv[0] = sender prefix
**      parv[1] = message text
*/
int     m_admins(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int     parc;
char    *parv[];
    {
        char    *message, *pv[4];

         if (check_registered(sptr))
                 return 0;

        message = parc > 1 ? parv[1] : NULL;

        if (BadPtr(message))
            {
                sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
                           me.name, parv[0], "ADCHAT");
                return 0;
            }
#ifdef ADMINCHAT
        if (MyClient(sptr) && !IsAdmin(sptr))
#else
        if (MyClient(sptr))
#endif
            {
                sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
                return 0;
            }
        sendto_serv_butone(IsServer(cptr) ? cptr : NULL,
                        ":%s ADCHAT :%s", parv[0], message);
#ifdef ADMINCHAT
	sendto_umode(UMODE_ADMIN, "*** AdminChat -- from %s: %s",
			parv[0], message);
	sendto_umode(UMODE_COADMIN, "*** AdminChat -- from %s: %s",
			parv[0], message);
#endif
        return 0;
    }
/*
** m_techat (Techadmin chat only) -Potvin (cloned by --sts)
**      parv[0] = sender prefix
**      parv[1] = message text
*/
int     m_techat(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int     parc;
char    *parv[];
    {
        char    *message, *pv[4];

         if (check_registered(sptr))
                 return 0;

        message = parc > 1 ? parv[1] : NULL;

        if (BadPtr(message))
            {
                sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
                           me.name, parv[0], "TECHAT");
                return 0;
            }
#ifdef ADMINCHAT
        if (MyClient(sptr))
           if  (!(IsTechAdmin(sptr) || IsNetAdmin(sptr)))
#else
        if (MyClient(sptr))
#endif
            {
                sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
                return 0;
            }
        sendto_serv_butone(IsServer(cptr) ? cptr : NULL,
                        ":%s TECHAT :%s", parv[0], message);
#ifdef ADMINCHAT
	sendto_umode(UMODE_TECHADMIN, "*** Te-chat -- from %s: %s",
			parv[0], message);
//        sendto_techat("from %s: %s", parv[0], message);
//		sendto_achat(1,"from %s: %s", parv[0], message);
#endif
        return 0;
    }
/*
** m_nachat (netAdmin chat only) -Potvin - another sts cloning
**      parv[0] = sender prefix
**      parv[1] = message text
*/
int     m_nachat(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int     parc;
char    *parv[];
    {
        char    *message, *pv[4];

         if (check_registered(sptr))
                 return 0;

        message = parc > 1 ? parv[1] : NULL;

        if (BadPtr(message))
            {
                sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
                           me.name, parv[0], "NACHAT");
                return 0;
            }
#ifdef ADMINCHAT
        if (MyClient(sptr))
		if (!(IsNetAdmin(sptr) || IsTechAdmin(sptr)))
#else
        if (MyClient(sptr))
#endif
            {
                sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
                return 0;
            }

        sendto_serv_butone(IsServer(cptr) ? cptr : NULL,
                        ":%s NACHAT :%s", parv[0], message);
#ifdef ADMINCHAT
	sendto_umode(UMODE_NETADMIN, "*** NetAdmin.Chat -- from %s: %s",
				parv[0], message);
	sendto_umode(UMODE_TECHADMIN, "*** NetAdmin.Chat -- from %s: %s",
				parv[0], message);	
#endif
        return 0;
    }

/* m_lag (lag measure) - Stskeeps
 * parv[0] = prefix
 * parv[1] = server to query
*/

int m_lag(cptr, sptr, parc, parv) 
aClient *cptr, *sptr;
int parc;
char *parv[];
{
	int i;
	if (check_registered_user(sptr))
    	return 0;
    
    if (MyClient(sptr))
		if (!IsAnOper(sptr)) {
                sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
                return 0;
		}
	
	if (parc < 2) {
         sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
                     me.name, parv[0], "LAG");
         return 0;
	}
	if (*parv[1] == '\0') {
         sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
                     me.name, parv[0], "LAG");
         return 0;
	}
	if (hunt_server(cptr, sptr, ":%s LAG :%s", 1, parc, parv) == HUNTED_NOSUCH) {
		return 0;
	}
	
	sendto_one(sptr, ":%s NOTICE %s :Lag reply -- %s %s %li",
		me.name, sptr->name,
		me.name, parv[1],
		TStime());

	return 0;
}

/*
 * m_dusers (dump users) (not passed on to other servers - Stskeeps
 * /DUSERS <mask>
 * parv[0] = prefix
 * parv[1] = mask
 * parv[2] = (optional) options
*/
#define DOPT_OPS 0x0001
#define DOPT_BOTS 0x0002
#define DOPT_MINE 0x0004

int     m_dusers(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int     parc;
char    *parv[];
{
	aClient	*acptr;
	long	ccount = 0;	
	int		opt = 0; /* 1 */
	char	*op;

	if (!IsAnOper(sptr)) {
   		 sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
         return 0;
	}
/* Will now say it is not available in what ever version it is */
	sendto_one(sptr, ":%s NOTICE %s :*** /Dusers is disabled in %s%s - sorry", me.name, sptr->name, BASE_VERSION, VERSIONONLY);
	return 0;

	if (parc < 2) {       
	     sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
                      me.name, parv[0], "DUSERS");
         return 0;
	}
	
	if (*parv[1] == '\0') {
	        sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
                       me.name, parv[0], "DUSERS");
            return 0;
	}
		
	if (parc < 4) { /* we got options .. */
		op = parv[2];
		while (*op != '\0') {
			switch (*op) {
				case '+': break;
				case 'o': opt |= DOPT_OPS; break;
				case 'B': opt |= DOPT_BOTS; break;
				case 'l': opt |= DOPT_MINE; break;
				default: sendto_one(sptr, ":%s NOTICE %s :Unknown /DUSERS option flag (%c)", me.name, sptr->name, *op);
			}
			op++;
		}		
	}
	sendto_one(sptr, rpl_str(RPL_DUMPING), me.name, parv[0], parv[1]);

	for (acptr = client; acptr; acptr = acptr->next) {
		if (IsClient(acptr)) {
			if (IsOper(acptr) && !(opt & DOPT_OPS))			
				continue;
			if (!(acptr->umodes & UMODE_BOT) && (opt & DOPT_BOTS))
				continue;
			if (!MyConnect(acptr) && (opt & DOPT_MINE))
				continue;	
				
			/* Complicated. */
			sendto_one(sptr, rpl_str(RPL_DUMPRPL), me.name, parv[0],
				acptr->name, acptr->user->username, 
				(IsHidden(acptr) ? acptr->user->virthost : acptr->user->realhost),
				acptr->user->serv->name,
				(IsOper(acptr) ? "o" : ""),
				((acptr->umodes & UMODE_BOT) ? "b" : ""),
				(IsServices(acptr) ? "s" : ""),
(!(IsOper(acptr) || (acptr->umodes & UMODE_BOT) || IsServices(acptr)) ? "n" : ""));
			ccount++;
		}
	}	
	sendto_one(sptr, rpl_str(RPL_EODUMP), me.name, parv[0], ccount);
}

/*
  Help.c interface for command line :)
*/
void	unrealmanual(void) {
	char	*str;
	int		x = 0;	

	str = MyMalloc(1024);	
	printf("Starting UnrealIRCD Interactive help system\n");
	printf("Use 'QUIT' as topic name to get out of help system\n");
	x = parse_help(NULL, NULL, NULL);
	if (x == 0) {
		printf("*** Couldn't find main help topic!!\n");
		return;
	}	
	while (myncmp(str, "QUIT", 8)) {
		printf("Topic?> ");
		str = fgets(str, 1023, stdin);
		printf("\n");
		if (myncmp(str, "QUIT", 8)) 		
		  x = parse_help(NULL, NULL, str);
	      if (x == 0) {
			 printf("*** Couldn't find help topic '%s'\n", str);
		  }
	}
	MyFree(str);
}	

static char *militime(char *sec, char *usec)
{
  struct timeval tv;
  static char timebuf[18];
#ifndef _WIN32  
  gettimeofday(&tv, NULL);
#else
	/* win32 unreal cannot fix gettimeofday - therefore only 90% precise */
	tv.tv_sec = TStime();
	tv.tv_usec = 0;
#endif
  if (sec && usec)
    sprintf(timebuf, "%ld",
	(tv.tv_sec - atoi(sec)) * 1000 + (tv.tv_usec - atoi(usec)) / 1000);
  else
    sprintf(timebuf, "%ld %ld", tv.tv_sec, tv.tv_usec);

  return timebuf;
}

aClient *find_match_server(char *mask)
{
  aClient *acptr;

  if (BadPtr(mask))
    return NULL;
  for (acptr = client, collapse(mask); acptr; acptr = acptr->next)
  {
    if (!IsServer(acptr) && !IsMe(acptr))
      continue;
    if (!match(mask, acptr->name))
      break;
    continue;
  }
  return acptr;
}

/*
 * m_rping  -- by Run
 *
 *    parv[0] = sender (sptr->name thus)
 * if sender is a person: (traveling towards start server)
 *    parv[1] = pinged server[mask]
 *    parv[2] = start server (current target)
 *    parv[3] = optional remark
 * if sender is a server: (traveling towards pinged server)
 *    parv[1] = pinged server (current target)
 *    parv[2] = original sender (person)
 *    parv[3] = start time in s
 *    parv[4] = start time in us
 *    parv[5] = the optional remark
 */
int m_rping(aClient *cptr, aClient *sptr, int parc, char *parv[])
{
  aClient *acptr;

  if (!IsPrivileged(sptr))
    return 0;

  if (parc < (IsAnOper(sptr) ? (MyConnect(sptr) ? 2 : 3) : 6))
  {
    sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS), me.name, parv[0], "RPING");
    return 0;
  }
  if (MyClient(sptr))
  {
    if (parc == 2)
      parv[parc++] = me.name;
    else if (!(acptr = find_match_server(parv[2])))
    {
      parv[3] = parv[2];
      parv[2] = me.name;
      parc++;
    }
    else
      parv[2] = acptr->name;
    if (parc == 3)
      parv[parc++] = "<No client start time>";
  }

  if (IsAnOper(sptr))
  {
    if (hunt_server(cptr, sptr, ":%s RPING %s %s :%s", 2, parc, parv) != HUNTED_ISME)
      return 0;
    if (!(acptr = find_match_server(parv[1])) || !IsServer(acptr))
    {
      sendto_one(sptr, err_str(ERR_NOSUCHSERVER), me.name, parv[0], parv[1]);
      return 0;
    }
      sendto_one(acptr, ":%s RPING %s %s %s :%s",
	  me.name, acptr->name, sptr->name, militime(NULL, NULL), parv[3]);
  }
  else
  {
    if (hunt_server(cptr, sptr, ":%s RPING %s %s %s %s :%s", 1, parc, parv) != HUNTED_ISME)
      return 0;
    sendto_one(cptr, ":%s RPONG %s %s %s %s :%s", me.name, parv[0],
	parv[2], parv[3], parv[4], parv[5]);
  }
  return 0;
}

/*
 * m_rpong  -- by Run too :)
 *
 * parv[0] = sender prefix
 * parv[1] = from pinged server: start server; from start server: sender
 * parv[2] = from pinged server: sender; from start server: pinged server
 * parv[3] = pingtime in ms
 * parv[4] = client info (for instance start time)
 */
int m_rpong(aClient *cptr, aClient *sptr, int parc, char *parv[])
{
  aClient *acptr;

  if (!IsServer(sptr))
    return 0;

  if (parc < 5)
  {
    sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
	me.name, parv[0], "RPING");
    return 0;
  }
 
  /* rping blowbug */
  if (!(acptr = find_client(parv[1], (aClient *)NULL)))
      return 0;


  if (!IsMe(acptr))
  {
    if (IsServer(acptr) && parc > 5)
    {
      sendto_one(acptr, ":%s RPONG %s %s %s %s :%s",
	  parv[0], parv[1], parv[2], parv[3], parv[4], parv[5]);
      return 0;
    }
  }
  else
  {
    parv[1] = parv[2];
    parv[2] = sptr->name;
    parv[0] = me.name;
    parv[3] = militime(parv[3], parv[4]);
    parv[4] = parv[5];
    if (!(acptr = find_person(parv[1], (aClient *)NULL)))
      return 0;			/* No bouncing between servers ! */
  }

  sendto_one(acptr, ":%s RPONG %s %s %s :%s",
      parv[0], parv[1], parv[2], parv[3], parv[4]);
  return 0;
}
/*
 * m_swhois
 * parv[1] = nickname
 * parv[2] = new swhois
 *
*/

int     m_swhois(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int     parc;
char    *parv[];
{
	aClient *acptr;
	
/*	if (!IsServer(sptr) && !IsULine(cptr, sptr) && !(IsNetAdmin(sptr) || IsTechAdmin(sptr)))
	{
		return 0;
	}*/
	if (!IsServer(sptr) && !IsULine(cptr, sptr))
		return 0;
	if (parc < 3) 
		return 0;
		
	acptr = find_person(parv[1], (aClient *) NULL);
	if (!acptr)
		return 0;
	
	if (acptr->user->swhois)
		MyFree(acptr->user->swhois);
	acptr->user->swhois = MyMalloc(strlen(parv[2]) + 1);
	sprintf(acptr->user->swhois, "%s", parv[2]);
	sendto_serv_butone(cptr, ":%s SWHOIS %s :%s", sptr->name, parv[1], parv[2]);
	return 0;	
}
/*
** m_sendumode - Stskeeps
**      parv[0] = sender prefix
**      parv[1] = target
**      parv[2] = message text
** Pretty handy proc.. 
** Servers can use this to f.x:
**   :server.unreal.net SENDUMODE F :Client connecting at server server.unreal.net port 4141 usw..
** or for sending msgs to locops.. :P
*/
int     m_sendumode(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int     parc;
char    *parv[];
    {
        char    *message, *pv[4];
	char *p;
         if (check_registered(sptr))
                 return 0;

        message = parc > 2 ? parv[2] : NULL;

        if (BadPtr(message))
            {
                sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
                           me.name, parv[0], "SENDUMODE");
                return 0;
            }

        if (!IsServer(sptr))
            {
                sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
                return 0;
            }

        sendto_serv_butone(IsServer(cptr) ? cptr : NULL,
                        ":%s SMO %s :%s", parv[0], parv[1], message);
	
	
	for (p = parv[1]; *p; p++)
	{
		switch (*p)
		{
			case 'e': sendto_umode(UMODE_EYES, "%s", parv[2]); break;
			case 'F':
				{
					if (*parv[2] != 'C' && *(parv[2]+1) != 'l')
						 sendto_umode(UMODE_FCLIENT, "%s", parv[2]); break;
				}
			case 'o': sendto_umode(UMODE_OPER, "%s", parv[2]); break;
			case 'O': sendto_umode(UMODE_LOCOP, "%s", parv[2]); break;
			case 'h': sendto_umode(UMODE_HELPOP, "%s", parv[2]); break;
			case 'N': sendto_umode(UMODE_NETADMIN|UMODE_TECHADMIN, "%s", parv[2]); break;
			case 'A': sendto_umode(UMODE_ADMIN, "%s", parv[2]); break;
			case '1': sendto_umode(UMODE_CODER, "%s", parv[2]); break;
			case 'I': sendto_umode(UMODE_HIDING, "%s", parv[2]); break;
			case 'w': sendto_umode(UMODE_WALLOP, "%s", parv[2]); break;
			case 's': sendto_umode(UMODE_SERVNOTICE, "%s", parv[2]); break;
			case 'T': sendto_umode(UMODE_TECHADMIN, "%s", parv[2]); break;
		}
	}
        return 0;
    }


/*
** m_tsctl - Stskeeps
**      parv[0] = sender prefix
**      parv[1] = command
**      parv[2] = options
*/

int     m_tsctl(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int     parc;
char    *parv[];
{
	time_t	timediff;
	
	if (check_registered(sptr))
                 return 0;

	if (!MyClient(sptr)) 
		goto doit;
	if (!IsAdmin(sptr) && !IsCoAdmin(sptr))
	{
                sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
                return 0;
	}
	doit:
	if (parv[1])
	{
		if (*parv[1] == '\0' )
		{
	                sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
        	                   me.name, parv[0], "TSCTL");
			return 0;
		}

		if (strcmp(parv[1], "offset")==0) {
			if (!parv[3])
			{
				sendto_one(sptr, ":%s NOTICE %s :*** TSCTL OFFSET: /tsctl offset <+|-> <time>", me.name, sptr->name);
				return 0;
			}
			if (*parv[2] == '\0' || *parv[3] == '\0')
			{
				sendto_one(sptr, ":%s NOTICE %s :*** TSCTL OFFSET: /tsctl offset <+|-> <time>", me.name, sptr->name);
				return 0;
			}				
			if (!(*parv[2] == '+' || *parv[2] == '-'))
			{
				sendto_one(sptr, ":%s NOTICE %s :*** TSCTL OFFSET: /tsctl offset <+|-> <time>", me.name, sptr->name);
				return 0;

			}
			
			switch (*parv[2])
			{
				case '+' :
						timediff = atol(parv[3]);
						TSoffset = timediff;
						sendto_ops("TS Control - %s set TStime() to be diffed +%li", sptr->name, timediff);					
						sendto_serv_butone(&me, ":%s GLOBOPS :TS Control - %s set TStime to be diffed +%li", me.name, sptr->name, timediff);
						break;						
				case '-' :
						timediff = atol(parv[3]);
						TSoffset = -timediff;
						sendto_ops("TS Control - %s set TStime() to be diffed -%li", sptr->name, timediff);					
						sendto_serv_butone(&me, ":%s GLOBOPS :TS Control - %s set TStime to be diffed -%li", me.name, sptr->name, timediff);
						break;						
			}
			return 0;
		}
		if (strcmp(parv[1], "time")==0)
		{
			sendto_one(sptr, ":%s NOTICE %s :*** TStime=%li time()=%li TSoffset=%li", me.name,
				sptr->name, TStime(), time(NULL), TSoffset);
			return 0;
		}
		if (strcmp(parv[1], "alltime")==0)
		{
			sendto_one(sptr, ":%s NOTICE %s :*** Server=%s TStime=%li time()=%li TSoffset=%li", me.name,
				sptr->name, me.name, TStime(), time(NULL), TSoffset);
			sendto_serv_butone(cptr, ":%s TSCTL alltime", sptr->name);
			return 0;
			
		}
		if (strcmp(parv[1], "svstime")==0)
		{
			if (parc < 3 || *parv[3] == '\0')
			{
				return 0;
			}
			if (!IsULine(cptr, sptr))
			{
				return 0;
			}
			
			timediff = atol(parv[3]);
			timediff = timediff - time(NULL);
			TSoffset = timediff;
			sendto_ops("TS Control - U:line set time to be %li (timediff: %li)", atol(parv[3]), timediff);
			sendto_serv_butone(cptr, ":%s TSCTL svstime %li", sptr->name, atol(parv[3]));
			return 0;
		}		
	}	
}


/*
** m_svso - Stskeeps
**      parv[0] = sender prefix
**      parv[1] = nick
**      parv[2] = options
*/

int     m_svso(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int     parc;
char    *parv[];
{
	aClient *acptr;
	long	fLag;
	
	if (check_registered(sptr))
                 return 0;
	if (!IsULine(cptr, sptr))
		return 0;
		
	if (parc < 3)
		return 0;
		
	if (!(acptr = find_client(parv[1], (aClient *)NULL)))
	    return 0;

	if (!MyClient(acptr))
	{
		sendto_one(acptr, ":%s SVSO %s %s", parv[0], parv[1], parv[2]);
		return 0;
	}
	
	if (*parv[2] == '-') 
	{
		fLag = acptr->umodes;
		acptr->umodes &= ~(UMODE_OPER|UMODE_LOCOP|UMODE_HELPOP|UMODE_SERVICES|UMODE_SADMIN|UMODE_ADMIN);
		acptr->umodes &= ~(UMODE_NETADMIN|UMODE_TECHADMIN|UMODE_CLIENT|UMODE_FLOOD|UMODE_EYES|UMODE_CHATOP|UMODE_WHOIS);
		acptr->umodes &= ~(UMODE_KIX|UMODE_FCLIENT|UMODE_HIDING|UMODE_AGENT|UMODE_CODER|UMODE_DEAF);		
		acptr->oflag = 0;
		send_umode_out(acptr, acptr, fLag);
	}	
}

int	m_shun(cptr, sptr, parc,parv)
aClient *cptr, *sptr;
int	parc;
char	*parv[];
{
}

int	m_htm(cptr, sptr, parc,parv)
aClient *cptr, *sptr;
int	parc;
char	*parv[];
{
	if (!IsOper(sptr))
		return 0;

#ifndef NO_FDLIST		
	sendto_one(sptr, ":%s NOTICE %s :*** Current incoming rate: %0.2f kb/s", me.name, sptr->name, currentrate);
	sendto_one(sptr, ":%s NOTICE %s :*** Current outgoing rate: %0.2f kb/s", me.name, sptr->name, currentrate2);
	sendto_one(sptr, ":%s NOTICE %s :*** Highest incoming rate: %0.2f kb/s", me.name, sptr->name, highest_rate);
	sendto_one(sptr, ":%s NOTICE %s :*** Highest outgoing rate: %0.2f kb/s", me.name, sptr->name, highest_rate2);
	sendto_one(sptr, ":%s NOTICE %s :*** High traffic mode is currently \2%s\2", me.name, sptr->name, (lifesux ? "ON" : "OFF"));
	sendto_one(sptr, ":%s NOTICE %s :*** HTM will be activated if incoming > %i kb/s", me.name, sptr->name, LOADRECV);
#else
	sendto_one(sptr, ":%s NOTICE %s :*** High traffic mode and fdlists are not enabled on this server", me.name, sptr->name);
#endif
}

