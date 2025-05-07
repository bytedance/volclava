#ifndef INCLUDE_LIB_OSUX_H
#define INCLUDE_LIB_OSUX_H

/* $Id: lib.osux.h 397 2007-11-26 19:04:00Z mblack $
 * Copyright (C) 2007 Platform Computing Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
 */

int                                     
procStart_( char* argv[],               
            char* envp[],               
            int   procStartFlags,       
            LS_PROC_START_T* startInfo, 
            LS_PROCESS_T* procInfo)     
{
    int ii;
    pid_t idPro;
    pid_t idCrnt;
    int flgStdHd[3];
    int  tmpPrio;
    struct passwd *upswd;

    if(argv == NULL || procInfo == NULL) {
        return ERR_BAD_ARG;
    }

    memset( procInfo, 0, sizeof(LS_PROCESS_T) );
    if(startInfo == NULL) {
         
	if((idPro = fork()) < 0) {
            return ERR_SYSERR;
	}
	else if(idPro == 0) { 
            if(procStartFlags & PROC_NOT_INH_HD_LS) {
		
		for(ii = sysconf(_SC_OPEN_MAX) ; ii >= 0 ; ii--)
			     close(ii);
            }
            if(procStartFlags & PROC_NEW_GROUP_LS) {
		
		idCrnt = getpid();
		if(-1 == setpgid (idCrnt, idCrnt)) return ERR_SYSERR;
            }

	    if(-1 ==  execve(argv[0], argv, envp)) {
                return ERR_SYSERR;
	    }
	}
    }
    else { 
         
	if((idPro = fork()) < 0) {
            return ERR_SYSERR;
	}
	else if(idPro == 0) { 
            if(procStartFlags & PROC_NOT_INH_HD_LS) {
		
		for(ii = sysconf(_SC_OPEN_MAX) ; ii >= 0 ; ii--)
			     close(ii);
            }
            else if(startInfo->numHds) {
                flgStdHd[0] = 0;
                flgStdHd[1] = 0;
                flgStdHd[2] = 0;

                for(ii=0; ii < startInfo->numHds; ii++) {
                    if(startInfo->flgHds[ii] & CHLD_KEEP_MSK_LS) {
                        
                        if((startInfo->flgHds[ii] & CHLD_KEEP_STDIN_LS) &&
                           flgStdHd[0] == 0) {
                            flgStdHd[0] = 1;
			    if(-1 == dup2(startInfo->handles[ii], 0)) {
                                return ERR_SYSERR;
                            }
                        }

                        if((startInfo->flgHds[ii] & CHLD_KEEP_STDOUT_LS) &&
                           flgStdHd[1] == 0) {
                            flgStdHd[1] = 1;
			    if(-1 == dup2(startInfo->handles[ii], 1)) {
                                return ERR_SYSERR;
                            }
                        }

                        if((startInfo->flgHds[ii] & CHLD_KEEP_STDERR_LS) &&
                           flgStdHd[2] == 0) {
                            flgStdHd[2] = 1;
			    if(-1 == dup2(startInfo->handles[ii], 2)) {
                                return ERR_SYSERR;
                            }
                        }
                    }
                    else {
                        
		        close(startInfo->handles[ii]);
                    }
                }
            }

	    idCrnt = getpid();
            if(procStartFlags & PROC_NEW_GROUP_LS) {
		
		if(-1 == setpgid (idCrnt, idCrnt)) {
		    return ERR_SYSERR;
		}
            }
            if(startInfo->priority) {
                 
		tmpPrio = PROC_OSPRIO_LS(startInfo->priority);
            }
            if(startInfo->wrkDir) {
                 
		if(-1 == chdir(startInfo->wrkDir)) {
		    return ERR_SYSERR;
		}
            }
            if(startInfo->userName) {
                
		if(NULL == (upswd = getpwlsfuser_(startInfo->userName))) {
		    return ERR_BAD_ARG;
		}
		if(-1 == (setuid(upswd->pw_uid))) {
		    return ERR_BAD_ARG;
		}
	    }

	    if(-1 ==  execve(argv[0], argv, envp)) {
                return ERR_SYSERR;
	    }
	}
	else { 
            if(startInfo->numHds) {
                for(ii=0; ii < startInfo->numHds; ii++) {
                }
            }
	}
    }

    procInfo->hdProc = idPro;
    procInfo->hdThrd = idPro;
    procInfo->idProc = idPro;
    procInfo->idThrd = idPro;
    procInfo->idProcGrp = 0;
    procInfo->userName = NULL;
    procInfo->wrkDir = NULL;

    procInfo->misc |= MSCAVA_PROC_LS;
    if(procStartFlags & PROC_JOB_PROC_LS) {
        procInfo->misc |= MSCJOB_PROC_LS;
    }

    return 0;
} 


int                                     
hdNotInherit_(LS_HANDLE_T *hd)          
{
    if(hd >= (LS_HANDLE_T *)0 && hd <= (LS_HANDLE_T *)2) {
        if(-1 == fcntl(*hd, F_SETFD, FD_CLOEXEC)) {
            return ERR_SYSERR;
        }
    }
    else {
        if(-1 == fcntl(*hd, F_SETFD, FD_CLOEXEC)) {
            return ERR_SYSERR;
        }
    }
    return 0;
}


int                                     
procLsInit_(int flags)                  
{
    return 0;
} 

int                                     
procSendSignal_(LS_ID_T idProc,         
                int sig,                
		int jobTerminateInterval, 
                int options)            
{
    if(idProc < 0) {
	return ERR_BAD_ARG;
    }
    else if(idProc) {
	if((options & SEND_GROUP_PROC) != 0) {
	    if(-1 == kill(-idProc, sig)) {
	        return ERR_SYSERR;
	    }
	}
	else {
	    if(-1 == kill(idProc, sig)) {
	        return ERR_SYSERR;
	    }
	}
    }
    else {
	if(-1 == kill(0, sig)) {
	    return ERR_SYSERR;
	}
    }

    return 0;
} 


int                                     
thrdStart_( void (*thread_func)(void*), 
            void *params,               
            int   thrdStartFlags,       
            LS_THRD_START_T* startInfo, 
            LS_THREAD_T* thrdInfo)      
{
    pid_t idPro;
    pid_t idCrnt;
    int  tmpPrio;

    if(thread_func == NULL || thrdInfo == NULL) {
        return ERR_BAD_ARG;
    }

     
    if((idPro = fork()) < 0) {
        return ERR_SYSERR;
    }
    else if(idPro == 0) { 
	idCrnt = getpid();
        if(startInfo->priority) {
             
	    tmpPrio = THRD_OSPRIO_LS(startInfo->priority);
        }
	(*thread_func)(params);
	exit(0);
    }
    else {
        thrdInfo->hdThrd = thrdInfo->idThrd = idPro;
        return 0;
    }
} 


char**                                  
getEnvStrs_()
{
    char* tmpStr;
    char** rtnStr;
    int ii, jj;

    for(ii=0, jj=0; environ[ii]; ii++) {
	jj += (strlen(environ[ii]) + 1);
    }

    rtnStr = (char**)malloc( sizeof(char*)*ii + sizeof(char)*jj );
    if(rtnStr == NULL) return NULL;
    tmpStr = (char*)&rtnStr[ii];

    for(ii=0; strcpy(tmpStr, environ[ii]); ii++) {
        rtnStr[ii] = tmpStr;
        tmpStr += (strlen(tmpStr)+1);
    }
    rtnStr[ii] = NULL;

    return rtnStr;
} 


int                                     
FreeEnvStrs_(char* envp[])              
{
    free(envp);
    return 0;
} 


unsigned long                           
getEnvVar_( char* name,                 
	    char* buf,                  
	    unsigned long size)         
{
    char* varStr;
    unsigned long len;

    if(NULL == (varStr = getenv(name))) return 0;

    len = strlen(varStr);
    if(size <= len) {
	return (len + 1);
    }
    else {
	if(buf == NULL) return (len + 1);
	strcpy(buf, varStr);
	return len;
    }
}

#endif /* INCLUDE_LIB_OSUX_H */


