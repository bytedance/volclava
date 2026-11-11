/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: erestart.c 397 2007-11-26 19:04:00Z mblack $
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

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <strings.h>
#include <stdlib.h>
#ifdef _TEST
#include <sys/stat.h>
#include <fcntl.h>
#endif 

#include "echkpnt.lib.h"
#include "echkpnt.env.h"

static void  sendPidPGid(pid_t,pid_t);
static int   getUserCmdFromJF(char *,const char *);
static int   getUserCmdFromCmdF(char *);
static int   isUserCmdLineMark(const char *);
static int   buildNewJobFile(char *,const char *,const char *);
static void  usage(const char *);

static char logMesgBuf[MAXLINELEN];

void 
usage (const char *pCmd)
{
    fprintf(stderr, "Usage: %s [-c] [-f] [-h] [-V] chkpntdir\n", pCmd);
    exit(-1);
}


static void 
sendPidPGid(pid_t pid, pid_t pgid){

	char  message[MAXLINELEN];
	sprintf(message,"%s%d %s%d",
            PID_MSG_HEADER, pid,
            PGID_MSG_HEADER, pgid);
	
	
	fprintf(stderr, "%s",message);
	fflush(stderr);					
}


int 
main(int argc, char **argv){
	static char fname[] = "main()";

	extern int optind; 
	 
	char  *pMethodName = NULL;
	char  *pMethodDir = NULL;
	char  *pIsOutput = NULL;
	char  *pOldJobID = NULL;
	char *presAcctFN = NULL;
	char  *pChkpntDir = NULL;				
	char  erestartProgPath[MAXPATHLEN];      		
	pid_t   iChildPid, myPid, myPGid;           		
	LS_WAIT_T  iStatus;					
	int   iReValue;
	char  *pStderrFdEnv = NULL;
	int   iStderrFd;
	char  *pJobFileName = NULL;
	char *cargv[MAX_ARGS];
	int  argIndex = 0;
	int  cc;
	long lMaxfds;

	
	char newUserCmd[MAXFILENAMELEN];
	char newJobFileName[MAXPATHLEN];
	char *pUserCmd = NULL;

	 
	while ((cc = getopt(argc, argv, "cfVh")) != EOF ){
		switch(cc){
		case 'c':
		case 'f':
			break;
		case 'V':
			fputs(_LS_VERSION_, stdout);
			exit(0);
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			usage(argv[0]);
			exit(-1);
		}
	}
	
	
	if (argc == optind + 1){
		pChkpntDir = argv[optind];
	}else{
		
		usage(argv[0]);
		exit(-1);
	}

	
        if (access(pChkpntDir,W_OK | X_OK) < 0){
                fprintf(stderr,"%s : the chkpnt dir %s can not be accessed\n",
                        fname,pChkpntDir);
		exit(-1);
        }

	
	
	iReValue = fileIsExist(NULL);
	if (iReValue == 1){

		
		pMethodName = getEchkpntVar(ECHKPNT_METHOD);
		
		if  ((pMethodName == NULL)||(strcmp(pMethodName,"") == 0)){
			pMethodName = ECHKPNT_DEFAULT_METHOD;
		}

		pMethodDir = getEchkpntVar(ECHKPNT_METHOD_DIR);
		pIsOutput = getEchkpntVar(ECHKPNT_KEEP_OUTPUT);
		pOldJobID = getEchkpntVar(ECHKPNT_OLD_JOBID);
		presAcctFN= getEchkpntVar(ECHKPNT_ACCT_FILE);	
	}else if (iReValue == 0) { 
		
		pMethodName = ECHKPNT_DEFAULT_METHOD;
	}else{
		fprintf(stderr, "%s : the .echkpnt file content occurs error: %s\n",fname,strerror(errno));
		exit(-1);
	}

	
 	if ((pIsOutput != NULL)&&((strcmp(pIsOutput,ECHKPNT_OPEN_OUTPUT) == 0)
	    ||(strcmp(pIsOutput,ECHKPNT_OPEN_OUTPUT_L) == 0))) {
		initLog("Erestart");
	}
	
	if (iReValue == 0){
		sprintf(logMesgBuf, "%s : the .echkpnt file does not exists during restarting job\n",fname);
		logMesg(logMesgBuf);
	}

#ifdef DEBUG
        sprintf(logMesgBuf,"%s : the LSB_ECHKPNT_METHOD = %s\n",fname,pMethodName);
        logMesg(logMesgBuf);
        sprintf(logMesgBuf,"%s : the LSB_ECHKPNT_METHOD_DIR = %s\n",
		fname,pMethodDir != NULL?pMethodDir : "");       
        logMesg(logMesgBuf);    
#endif
 
	 
	if (getEchkpntMethodDir(erestartProgPath, pMethodDir, ERESTART_PROGRAM, pMethodName) == NULL){
		sprintf(logMesgBuf, "%s : the erestart method(%s) path is not correct\n",fname,pMethodName);
		goto Error;
	}
            
#ifdef DEBUG
	sprintf(logMesgBuf,"%s : the erestartProgPath is : %s\n",fname,erestartProgPath);
	logMesg(logMesgBuf);
#endif
	for(argIndex = 0;argIndex < argc; argIndex++){
		cargv[argIndex] = argv[argIndex];
	}
	cargv[argIndex] = NULL;
		
	 
    	if (strcmp(pMethodName, ECHKPNT_DEFAULT_METHOD) == 0){
		cargv[0] = "erestart.default";
		freeTable_();
#ifdef DEBUG
		logMesg("the erestart.default will be executed\n");
#endif
		closeLog();
		execv(erestartProgPath, cargv);
		sprintf(logMesgBuf,"%s : execute the erestart.default fail\n%s\n",fname,
			(errno? strerror(errno):"")); 
		fprintf(stderr,"%s",logMesgBuf);
		exit(-1);
	} 

	
	myPid = getpid();
	if (myPid == -1){
		sprintf(logMesgBuf,"%s : getpid() failed in erestart: %s\n",fname,strerror(errno));
		goto Error;
	}
	myPGid = getpgrp();
	if (myPGid == -1){
		sprintf(logMesgBuf,"%s : getpgid() failed in erestart: %s\n",fname,strerror(errno));
		goto Error;
	}

	sendPidPGid(myPid, myPGid);

	
#ifdef _TEST 
	{
	char strfd[MAXLINELEN];
	int fd;
	fd = open("/export/home/cchen/null.out",O_CREAT|O_WRONLY,ECHKPNT_FILE_MODE);
	strerror(errno);
	printf("the fd is %d\n",fd);
	sprintf(strfd,"LSB_STDERR_FD=%d",fd);
	putenv(strfd);
	}
#endif

	
	pJobFileName = getenv(ECHKPNT_JOBFILENAME);
	
	if (pJobFileName == NULL){
		sprintf(logMesgBuf,"%s : LSB_CHKFILENAME can not be gotten from environment\n",fname);
		goto Error;
	}

#ifdef DEBUG
	sprintf(logMesgBuf,"%s : pJobFileName is %s\n",fname,pJobFileName);
	logMesg(logMesgBuf);
#endif

	 
	iChildPid = fork();
	if (iChildPid < 0){
		sprintf(logMesgBuf, "%s : fork() fork a child process fail...\n",fname);
		goto Error;
	
	
	}else if (iChildPid == 0){
		char progName[MAXFILENAMELEN];
		char userCmd[MAXFILENAMELEN];
		int  ii;

		
		sprintf(logMesgBuf,"erestart.%s",pMethodName);
		setMesgHeader(logMesgBuf);

		
		if (getUserCmdFromJF(userCmd,pJobFileName) == 0){
			putEnv(ECHKPNT_RESTART_USRCMD, userCmd);
#ifdef DEBUG
			sprintf(logMesgBuf,"%s : the user command in the job file is %s\n",fname,userCmd);
			logMesg(logMesgBuf);
#endif

		}else{
			sprintf(logMesgBuf,"%s : the getUserCmdFromJF() could not get the user command from the job file %s\n",
				fname,pJobFileName); 
			goto Error;
		}
		
		
		if ((pOldJobID != NULL) && (strcmp(pOldJobID,"") != 0)){
			putEnv(ECHKPNT_OLD_JOBID, pOldJobID);
		}else{
			sprintf(logMesgBuf,"%s : the env var LSB_OLD_JOBID  could not gotten from the .echkpnt file\n",fname);
			goto Error;
		}
		
		
		if ((pIsOutput == NULL) || ((strcmp(pIsOutput,ECHKPNT_OPEN_OUTPUT) != 0)
		    && (strcmp(pIsOutput,ECHKPNT_OPEN_OUTPUT_L) != 0 ))){
			if (redirectFd(ECHKPNT_DEFAULT_OUTPUT_FILE, 1) == -1){
				sprintf(logMesgBuf, "%s : redirect stdout to %s file fail\n%s\n", 
					fname, ECHKPNT_DEFAULT_OUTPUT_FILE, 
					errno? strerror(errno) : "");
				goto Error;
			}
			if (redirectFd(ECHKPNT_DEFAULT_OUTPUT_FILE, 2) == -1){
				sprintf(logMesgBuf, "%s : redirect stderr to %s file fail\n%s\n", 
					fname, ECHKPNT_DEFAULT_OUTPUT_FILE, 
					errno? strerror(errno) : "");
				goto Error;
			}
		
		}else{
			char aFileName[MAXPATHLEN];
			
			if ((getChkpntDirFile(aFileName, ERESTART_STDOUT_FILE) == -1)
			    ||  (redirectFd(aFileName,1) == -1)){
				sprintf(logMesgBuf,"%s : redirect the stdout to %s fail\n",fname,ERESTART_STDOUT_FILE);
				logMesg(logMesgBuf);
				
				if (redirectFd(ECHKPNT_DEFAULT_OUTPUT_FILE, 1) == -1){
					sprintf(logMesgBuf, "%s : redirect stdout to %s fail\n%s\n", 
						fname, ECHKPNT_DEFAULT_OUTPUT_FILE, 
						errno? strerror(errno) : "");
					goto Error;
				}
			}
			
			
			if ((getChkpntDirFile(aFileName, ERESTART_STDERR_FILE) == -1)
			    ||  (redirectFd(aFileName,2) == -1)){
				sprintf(logMesgBuf,"%s : redirect the stderr to %s fail\n",fname,ERESTART_STDERR_FILE);
				logMesg(logMesgBuf);
				if (redirectFd(ECHKPNT_DEFAULT_OUTPUT_FILE, 2) == -1){
					fprintf(stderr, "%s : redirect stderr to %s fail\n%s\n", 
						fname, ECHKPNT_DEFAULT_OUTPUT_FILE, 
						errno? strerror(errno) : "");
					goto Error;
				}
			}
		}
		
		lMaxfds = sysconf(_SC_OPEN_MAX);
		for (ii = 3; ii < lMaxfds; ii++){
			close(ii);
		}

		sprintf(progName,"%s.%s",ERESTART_PROGRAM,pMethodName);
		cargv[0] = progName;
		freeTable_();
		
		execv(erestartProgPath,cargv);
		sprintf(logMesgBuf, "%s : the child process execute the %s fail\n",fname,progName); 
		fprintf(stderr,"%s",logMesgBuf);
		logMesg(logMesgBuf);
		closeLog();
		exit(-1);
	} 
  
	

	while ((iChildPid = waitpid(iChildPid, &iStatus, 0)) < 0 && errno == EINTR);

	
	if (iChildPid < 0 ){
		sprintf(logMesgBuf, "%s : %s fail, \n%s\n", 
			fname, "waitpid", errno? strerror(errno) : "");
		goto Error;
	}else{
		if ((WEXITSTATUS(iStatus)) != 0) {
			sprintf(logMesgBuf, "%s : the erestart.%s fail\n", fname, pMethodName);
			fprintf(stderr,"%s",logMesgBuf);
			logMesg(logMesgBuf);
			freeTable_();
			closeLog();
			exit(WEXITSTATUS(iStatus));
		}
	}
	
	
	
	
	if ((presAcctFN != NULL) && (strcmp(presAcctFN,"") != 0)){
	   	putEnv(ECHKPNT_ACCT_FILE, presAcctFN);
	} else {
		sprintf(logMesgBuf, "%s : the environment variable LSB_ACCT_FILE could not be gotten from the .echkpnt file \n", fname);
		goto Error;
        }
	iReValue = getUserCmdFromCmdF(newUserCmd);
	switch(iReValue){
	case 1:
		pUserCmd = newUserCmd;
		break;
	case 0:
		pUserCmd = NULL;
		sprintf(logMesgBuf,"%s : the .restart_cmd does not exist\n",fname);
		logMesg(logMesgBuf);
		break;
	default: 
		sprintf(logMesgBuf,"%s : there are some errors during getting the restart command from .restart_cmd file\n",
			fname);
		goto Error;
	}
			
#ifdef DEBUG 
	if (pUserCmd != NULL){
		sprintf(logMesgBuf,"%s : the new user command is %s\n",fname,pUserCmd);
		logMesg(logMesgBuf);
	}
#endif

	
    	if (buildNewJobFile(newJobFileName, pUserCmd, pJobFileName) != 0){
		sprintf(logMesgBuf,"%s : buildNewJobFile() can not build new job file\n%s\n",fname,
			errno? strerror(errno) : "");
       		goto Error;
	}

#ifdef DEBUG
	sprintf(logMesgBuf,"%s : the new job file name is %s\n",fname,newJobFileName);
	logMesg(logMesgBuf);
#endif

	
	iChildPid = fork();
	if (iChildPid < 0){
		sprintf(logMesgBuf, "%s : fork a child process fail...\n%s\n",fname,
			errno? strerror(errno) : "");
		goto Error;
	}else if (iChildPid == 0){
		char *pSubCwd = NULL;
		char fileName[MAXFILENAMELEN];
		char *pCurChar = NULL;
		int  ii;
		
		setMesgHeader("Real Job");

		pCurChar = newJobFileName;

		
		pSubCwd = getenv("LS_SUBCWD");
		if (chdir(pSubCwd) != 0){
			sprintf(logMesgBuf,"%s : chdir() change current working directory to %s fail\n%s\n",
				fname,(pSubCwd == NULL)?"":pSubCwd, strerror(errno));
			logMesg(logMesgBuf);
		}

		
		pCurChar = strrchr(pCurChar,'/');
		if (pCurChar != NULL){
			pCurChar++;
		}


		strcpy(fileName,pCurChar);	

#ifdef DEBUG
		sprintf(logMesgBuf,"%s : the new job file name is %s\n",fname,fileName);
		logMesg(logMesgBuf);
#endif

		cargv[0] = fileName;
		cargv[1] = NULL;

		
		pStderrFdEnv = getenv(ECHKPNT_STDERR_FD);
		if (pStderrFdEnv == NULL){
			sprintf(logMesgBuf,"%s : env var %s is not defined\n",fname,ECHKPNT_STDERR_FD);
			goto Error;
		}
		errno = 0;
		iStderrFd = strtol(pStderrFdEnv,NULL,10);
		if (errno != 0){
			sprintf(logMesgBuf,"%s : env variable %s is not correct\n",fname,ECHKPNT_STDERR_FD);
			goto Error;
		}

		dup2(iStderrFd, 2);

		
		lMaxfds = sysconf(_SC_OPEN_MAX);
        	for (ii = 3; ii < lMaxfds; ii++){
			close(ii);
		}

		freeTable_();

		
		execv(newJobFileName, cargv);
		sprintf(logMesgBuf, "%s : execute the %s fail\n",fname,newJobFileName); 
		fprintf(stderr,"%s",logMesgBuf);
		logMesg(logMesgBuf);
		closeLog();
		exit(-1);
	}
	
	
    	
	while ((iChildPid = waitpid(iChildPid, &iStatus, 0)) < 0 && errno == EINTR);

	if (iChildPid < 0 ) {
		sprintf(logMesgBuf, "%s : %s fail, \n%s\n", 
				fname, "waitpid", errno? strerror(errno) : "");
		goto Error;            
	}

#ifdef DEBUG
	sprintf(logMesgBuf,"%s : the real job exit value is %d\n",fname,WEXITSTATUS(iStatus));
	logMesg(logMesgBuf);
#endif

	
	if (unlink(newJobFileName) != 0){
		sprintf(logMesgBuf,"%s : can not delete the job file %s\n%s\n",fname,newJobFileName,strerror(errno));
		logMesg(logMesgBuf);
	}
	freeTable_();
	closeLog();
	exit(WEXITSTATUS(iStatus));

Error:

	fprintf(stderr,"%s",logMesgBuf);
	logMesg(logMesgBuf);
	freeTable_();
	closeLog();
	exit(-1);

}


static int
getUserCmdFromJF(char *pUserCmd, const char *pJobFileName){
	static char fname[] = "getUserCmdFromJF()";

	char  line[MAXLINELEN];
	FILE  *pFile = NULL;
   	int iCmdFlag = 0;
	
	
	pFile = fopen(pJobFileName,"r");
	if (pFile == NULL){
		sprintf(logMesgBuf,"%s : fopen() can not open the %s file\n%s\n",fname,pJobFileName,strerror(errno));
		logMesg(logMesgBuf);
		return(-1);
	}
	
	
	while (fgets(line,MAXLINELEN,pFile) != NULL) {
		char  *pCurChar = line;
		
		while (*pCurChar == ' '){
			pCurChar++;
		}
		
		switch(*pCurChar){
		case  '#' :
			
			if (isUserCmdLineMark(pCurChar) == 0){
				iCmdFlag = 1;
			}else{
				iCmdFlag = 0;
			}
			break;
		case '\0':
		case '\n':
			break;
		default:
			
			if (iCmdFlag){
				int len = strlen(pCurChar);
				if (pCurChar[len-1] == '\n'){
					pCurChar[len-1]='\0';
				}
				strcpy(pUserCmd,pCurChar);
				return(0);
			}else{
				iCmdFlag = 0;
			}
			break;
		}
	}
	return(-1);
}


static int 
isUserCmdLineMark(const char *pLine){

	char  aOperLine[MAXLINELEN];
	static char *apWord[3] ={
		"LSBATCH:",
		"User",
		"input"
	};
	int  ii;
	char *pCurChar = aOperLine;
	char *pCurWord = NULL;

	strcpy(aOperLine, pLine);
	ii = strlen(aOperLine);
	if (pCurChar[ii -1] == '\n'){
		pCurChar[ii -1] = '\0';
	}
	if (*pCurChar != '#'){
		return(-1);
	}
	
	
	for (ii = 0;(ii < 3) && (pCurChar != NULL); ii++ ){
		
		while (*(++pCurChar) == ' ');
		pCurWord = pCurChar;
		
		
		pCurChar = strchr(pCurWord,' ');
		if (pCurChar != NULL){
			*pCurChar = '\0';
		}
		if (strcmp(pCurWord, apWord[ii]) != 0){
			return(-1);
		}
	}
	if (ii != 3){
		return(-1);
	}
	if (pCurChar != NULL){
		
		while(*(++pCurChar) == ' ');
		if (*pCurChar != '\0'){
			return(-1);
		}
	}
	return(0);
}


static int
getUserCmdFromCmdF(char *pNewUserCmd){
	static char fname[] = "getUserCmdFromCmdF()";
	
	char  line[MAXLINELEN];
	char  cmdFileName[MAXPATHLEN];
	FILE  *pFile = NULL;
	int   iReValue = -1;

	
	if (getChkpntDirFile(cmdFileName,ECHKPNT_RESTART_CMD_FILE) != 0){
		sprintf(logMesgBuf,"%s : getChkpntDirFile call fail, can not combine the %s with chkpnt dir\n",
			fname,ECHKPNT_RESTART_CMD_FILE);
		logMesg(logMesgBuf);
		return(-1);
	}
		
	
	pFile = fopen(cmdFileName,"r");
	if (pFile == NULL){
		
		if (errno == ENOENT){
			return(0);
		}else{
			sprintf(logMesgBuf,"%s : fopen() can not open the %s file\n%s\n",fname,cmdFileName,strerror(errno));
			logMesg(logMesgBuf);
			return(-1);
		}
	}else{
		sprintf(logMesgBuf,"%s : the .restart_cmd exists \n",fname);
		logMesg(logMesgBuf);
	}

	
	while (fgets(line,MAXLINELEN,pFile) != NULL) {
		char  *pCurChar = line;
		char  *pCurWord = NULL;
		
		
		while (*(pCurChar) == ' '){
			pCurChar++;
		}

		
		pCurWord = strstr(pCurChar, ECHKPNT_RESTART_CMD_MARK);
		
		if ((pCurWord != NULL) && (pCurWord == pCurChar)){
			pCurChar = strchr(pCurChar,'=');
			while(*(++pCurChar) == ' ');
			
			strcpy(pNewUserCmd,pCurChar);
			iReValue = 1;
			break;
		}else{
			sprintf(logMesgBuf,"%s : the .restart_cmd content has some errors : %s\n",fname,line);
			logMesg(logMesgBuf);
			iReValue = -1;
			break;
		}
	}
	fclose(pFile);
	if (unlink(cmdFileName) != 0){
		sprintf(logMesgBuf,"%s : unlink() can not delete the %s file\n%s\n",
			fname,cmdFileName,strerror(errno));
		logMesg(logMesgBuf);
	}
	return(iReValue);
}


static int 
buildNewJobFile(char *pNewJobFileName,const char *pUserCmd, const char *pJobFileName){
	static char fname[] = "buildNewJobFile()";
	
	char  line[MAXLINELEN];
	FILE *pSource, *pDest;
	int  iCmdFlag = 0;

	
	if ((pJobFileName == NULL) || (pNewJobFileName == NULL)){
		return(-1);
	}
	strcpy(pNewJobFileName,pJobFileName);
	strcat(pNewJobFileName,ECHKPNT_NEWJOBFILE_SUFFIX);
    
	
	pSource = fopen(pJobFileName, "r");
	if (pSource == NULL){
		sprintf(logMesgBuf,"%s : fopen() can not open the %s file\n%s\n",
			fname,pJobFileName,strerror(errno));
		logMesg(logMesgBuf);
		return(-1);
	}
	pDest = fopen(pNewJobFileName, "w");
	if (pDest == NULL){
		sprintf(logMesgBuf,"%s : fopen() can not create the %s file\n%s\n",
			fname,pNewJobFileName,strerror(errno));
		logMesg(logMesgBuf);
		return(-1);
	}

	
	while (fgets(line,MAXLINELEN,pSource) != NULL) {
		char  *pCurChar = line;
		
		
		while (*pCurChar == ' '){
			pCurChar++;
		}
		
		switch(*pCurChar){
		case  '#' :
			
			if (isUserCmdLineMark(pCurChar) == 0){
				iCmdFlag = 1;
			}else{
				iCmdFlag = 0;
			}
			break;
		case '\0':
		case '\n':
			
			break;
		default:
			
			if (iCmdFlag){
				iCmdFlag = 0;
				if (pUserCmd == NULL){
					break;
				}else{
					fputs(pUserCmd,pDest);
					fprintf(pDest,"\n");
					continue;
				}

			
			}else{
				iCmdFlag = 0;
			}
			break;
		}
		fputs(line,pDest);
	}
	fclose(pSource);
	fclose(pDest);
	if (chmod(pNewJobFileName,S_IRWXU) != 0){
		sprintf(logMesgBuf,"%s : chmod() can not change the execute mode of the %s file\n%s\n",
			fname,pNewJobFileName,strerror(errno));
		logMesg(logMesgBuf);
		unlink(pNewJobFileName);
		return(-1);
	}
	return(0);
}
