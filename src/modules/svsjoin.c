/*
 *   Unreal Internet Relay Chat Daemon, src/modules/svsjoin.c
 *   (C) 2000-2001 Carsten V. Munk and the UnrealIRCd Team
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

#include "unrealircd.h"

CMD_FUNC(cmd_svsjoin);

/* Place includes here */
#define MSG_SVSJOIN       "SVSJOIN"

ModuleHeader MOD_HEADER
  = {
	"svsjoin",	/* Name of module */
	"5.0", /* Version */
	"command /svsjoin", /* Short description of module */
	"UnrealIRCd Team",
	"unrealircd-5",
    };

/* This is called on module init, before Server Ready */
MOD_INIT()
{
	CommandAdd(modinfo->handle, MSG_SVSJOIN, cmd_svsjoin, MAXPARA, CMD_USER|CMD_SERVER);
	MARK_AS_OFFICIAL_MODULE(modinfo);
	return MOD_SUCCESS;
}

/* Is first run when server is 100% ready */
MOD_LOAD()
{
	return MOD_SUCCESS;
}

/* Called when module is unloaded */
MOD_UNLOAD()
{
	return MOD_SUCCESS;	
}

/* cmd_svsjoin() - Lamego - Wed Jul 21 20:04:48 1999
   Copied off PTlink IRCd (C) PTlink coders team.
	parv[1] - nick to make join
	parv[2] - channel(s) to join
	parv[3] - (optional) channel key(s)
*/
CMD_FUNC(cmd_svsjoin)
{
	Client *acptr;

	if (!IsULine(sptr))
		return;

	if ((parc < 3) || !(acptr = find_person(parv[1], NULL)))
		return;

	if (MyUser(acptr))
	{
		parv[0] = acptr->name;
		parv[1] = parv[2];
		if (parc == 3)
		{
			parv[2] = NULL;
			(void)do_cmd(acptr, NULL, "JOIN", 2, parv);
			/* NOTE: 'acptr' may be killed if we ever implement spamfilter join channel target */
		} else {
			parv[2] = parv[3];
			parv[3] = NULL;
			(void)do_cmd(acptr, NULL, "JOIN", 3, parv);
			/* NOTE: 'acptr' may be killed if we ever implement spamfilter join channel target */
		}
	}
	else
	{
		if (parc == 3)
			sendto_one(acptr, NULL, ":%s SVSJOIN %s %s", sptr->name,
			    parv[1], parv[2]);
		else
			sendto_one(acptr, NULL, ":%s SVSJOIN %s %s %s", sptr->name,
				parv[1], parv[2], parv[3]);
	}
}
