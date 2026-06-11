/*
 * Copyright (C) 2021-2026 Bytedance Ltd. and/or its affiliates
 *
 * $Id: lsi18n.h 397 2007-11-26 19:04:00Z mblack $
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


#ifndef _LS_I18N_H
#define _LS_I18N_H

#include <sys/types.h>
#define LS_CATD int

#define I18N_CAT_MIN			1
#define I18N_CAT_LIM			1
#define I18N_CAT_PIM			2
#define I18N_CAT_RES			3
#define I18N_CAT_MBD			4
#define I18N_CAT_SBD			5
#define I18N_CAT_CMD			6
#define I18N_CAT_MAX			6

#define I18N_CATFILE			"lsf"

#define MOD_LSBATCH			4
#define MOD_LSB_BACC			5
#define MOD_LSB_BHIST			6
#define MOD_LSB_BSTATS			7
#define MOD_LSB_CMD			8
#define MOD_LSBD_MBD			10
#define MOD_LSBD_SBD			11
#define MOD_LSBD_MISC			12
#define MOD_LSB_LIB			13
#define MOD_LSF				21
#define MOD_LSF_INTLIB			22
#define MOD_LSF_LIB			23
#define MOD_LSF_LIM			24
#define MOD_LSF_LSADM			25
#define MOD_LSF_LSTOOLS			27
#define MOD_LSF_PIM			28
#define MOD_LSF_RES			29
#define MOD_MISC			33
#define MOD_TIME_FORMAT			35

#ifdef NL_SETN
#undef  NL_SETN 
#endif
#define NL_SETN                      33  

#define I18N_m(msgID, msg)\
  (_i18n_msg_get(ls_catd, 33, msgID, msg))

     

#define I18N_FUNC_FAIL \
    (I18N_m(1, "%s: %s() failed."))/*catgets1*/
#define I18N_FUNC_FAIL_M \
    (I18N_m(2, "%s: %s() failed, %m."))/*catgets2*/
#define I18N_FUNC_FAIL_MM \
    (I18N_m(3, "%s: %s() failed, %M."))/*catgets3*/
#define I18N_FUNC_FAIL_S \
    (I18N_m(4, "%s: %s() failed, %s."))/*catgets4*/
#define I18N_FUNC_S_FAIL \
    (I18N_m(5, "%s: %s(%s) failed."))/*catgets5*/
#define I18N_FUNC_S_FAIL_M \
    (I18N_m(6, "%s: %s(%s) failed, %m."))/*catgets6*/
#define I18N_FUNC_S_FAIL_MM \
    (I18N_m(7, "%s: %s(%s) failed, %M."))/*catgets7*/
#define I18N_FUNC_D_FAIL \
    (I18N_m(8, "%s: %s(%d) failed."))/*catgets8*/
#define I18N_FUNC_D_FAIL_M \
    (I18N_m(9, "%s: %s(%d) failed, %m."))/*catgets9*/
#define I18N_FUNC_D_FAIL_MM \
    (I18N_m(10, "%s: %s(%d) failed, %M."))/*catgets10*/
#define I18N_JOB_FAIL_S \
    (I18N_m(11, "%s: Job <%s> failed in %s()."))/*catgets11*/
#define I18N_JOB_FAIL_M \
    (I18N_m(12, "%s: Job <%s> failed, %m."))/*catgets12*/
#define I18N_JOB_FAIL_MM \
    (I18N_m(13, "%s: Job <%s> failed, %M."))/*catgets13*/
#define I18N_QUEUE_FAIL \
    (I18N_m(14, "%s: Queue <%s> failed in %s()."))/*catgets14*/
#define I18N_HOST_FAIL \
    (I18N_m(15, "%s: Host <%s> failed in %s()."))/*catgets15*/
#define I18N_CALENDAR_FAIL \
    (I18N_m(16, "%s: Calendar <%s> failed in %s()."))/*catgets16*/
#define I18N_GROUP_FAIL \
    (I18N_m(17, "%s: Group <%s> failed in %s()."))/*catgets17*/
#define I18N_ERROR \
    (I18N_m(18, "%s: Error in %s()."))/*catgets18*/
#define I18N_ERROR_LD \
    (I18N_m(19, "%s error %ld."))/*catgets19*/
#define I18N_S_ERROR_LD \
    (I18N_m(20, "%s: %s error %ld."))/*catgets20*/
#define I18N_CANNOT_OPEN \
    (I18N_m(21, "%s: Cannot open '%s' %m."))/*catgets21*/
#define I18N_FUNC_FAIL_ENO_D \
    (I18N_m(22, "%s: %s() failed, errno= %d.")) /*catgets22*/
#define I18N_CLOSED_S \
    (I18N_m(23, "Closed %s."))/*catgets23*/
#define I18N_OPEN_S \
    (I18N_m(24, "Open %s."))/*catgets24*/
#define I18N_SHOW_S \
    (I18N_m(25, "Show %s."))/*catgets25*/
#define I18N_LSADMIN_S \
    (I18N_m(26, "lsadmin %s."))/*catgets26*/
#define I18N_BADMIN_S \
    (I18N_m(27, "badmin %s."))/*catgets27*/
#define I18N_START_S \
    (I18N_m(28, "Startup %s."))/*catgets28*/
#define I18N_RESTART_S \
    (I18N_m(29, "Restart %s."))/*catgets29*/
#define I18N_SHUTDOWN_S \
    (I18N_m(30, "Shutdown %s."))/*catgets30*/
#define I18N_FUNC_FAIL_EMSG_S \
    (I18N_m(31, "%s: %s() failed, errmsg: %s."))/*catgets31*/
#define I18N_FUNC_S_FAIL_EMSG_S \
    (I18N_m(32, "%s: %s(%s) failed, errmsg: %s."))/*catgets32*/
#define I18N_PREMATURE_EOF \
    (I18N_m(33, "%s: %s(%d): Premature EOF in section %s"))/*catgets33*/
#define I18N_FUNC_D_FAIL_S \
    (I18N_m(34, "%s: %s(%d) failed, %s."))/*catgets34*/
#define I18N_FUNC_S_S_FAIL_M \
    (I18N_m(35, "%s: %s(%s, %s) failed, %m."))/*catgets35*/
#define I18N_FUNC_S_D_FAIL_M \
    (I18N_m(36, "%s: %s(%s, %d) failed, %m."))/*catgets36*/
#define I18N_FUNC_S_S_FAIL_S \
    (I18N_m(37, "%s(%s) failed: %s."))/*catgets37*/
#define I18N_FUNC_FAILED \
    (I18N_m(38, "%s failed."))/*catgets38*/
#define I18N_FUNC_D_D_FAIL_M \
    (I18N_m(39, "%s: %s(%d, %d) failed, %m."))/*catgets39*/
#define I18N_HORI_NOT_IMPLE  \
    (I18N_m(40, "%s: File %s at line %d: Horizontal %s section not implemented yet; use vertical format; section ignored"))/*catgets40*/
#define I18N_JOB_FAIL_S_S_M \
    (I18N_m(41, "%s: Job <%s> failed in %s(%s), %m."))/*catgets41*/
#define I18N_JOB_FAIL_S_M \
    (I18N_m(42, "%s: Job <%s> failed in %s(), %m."))/*catgets42*/
#define I18N_FUNC_S_ERROR \
    (I18N_m(43, "%s Error !"))/*catgets43*/
#define I18N_JOB_FAIL_S_D_M \
    (I18N_m(44, "%s: Job <%s> failed in %s(%d), %m."))/*catgets44*/
#define I18N_JOB_FAIL_S_MM \
    (I18N_m(45, "%s: Job <%s> failed in %s(), %M."))/*catgets45*/
#define I18N_JOB_FAIL_S_S_MM \
    (I18N_m(46, "%s: Job <%s> failed in %s(%s), %M."))/*catgets46*/
#define I18N_JOB_FAIL_S_S \
    (I18N_m(47, "%s: Job <%s> failed in %s(%s)."))/*catgets47*/
#define I18N_FUNC_S_S_FAIL \
    (I18N_m(48, "%s: %s(%s, %s) failed."))/*catgets48*/
#define I18N_PARAM_NEXIST \
    (I18N_m(49, "%s: parameter '%s' does not exist in '%s'."))/*catgets49*/
#define I18N_NO_MEMORY \
    (I18N_m(50, "No enough memory.\n"))/*catgets50*/
#define I18N_NOT_ROOT  \
    (I18N_m(51, "%s: Not root, cannot collect kernal info"))/*catgets51*/
#define I18N_NEG_RUNQ  \
    (I18N_m(52, "%s: negative run queue length: %f"))/*catgets52*/
#define I18N_FUNC_FAIL_NO_PERIOD \
    (I18N_m(53, "%s: %s() failed"))/*catgets53*/
#define I18N_FUNC_S_FAIL_MN \
    (I18N_m(54, "%s: %s(%s) failed, %k."))/*catgets54*/
#define I18N_FUNC_FAIL_NN \
    (I18N_m(55, "%s: %s() failed, %N."))/*catgets55*/
#define I18N_FUNC_FAIL_MN \
    (I18N_m(56, "%s: %s() failed, %k."))/*catgets56*/

#define I18N_All \
     (I18N_m(1000,"All"))         /*catgets1000*/
#define I18N_Array__Name \
     (I18N_m(1001,"Array_Name"))  /*catgets1001*/
#define I18N_Apply \
     (I18N_m(1002,"Apply"))       /*catgets1002*/
#define I18N_activated \
     (I18N_m(1003,"activated"))   /*catgets1003*/
#define I18N_active \
     (I18N_m(1004,"active"))      /*catgets1004*/
#define I18N_ATTRIBUTE \
     (I18N_m(1005,"ATTRIBUTE"))   /*catgets1005*/
#define I18N_Advanced \
     (I18N_m(1006,"Advanced"))    /*catgets1006*/
#define I18N_ACKNOWLEDGED \
     (I18N_m(1007,"ACKNOWLEDGED"))/*catgets1007*/
#define I18N_Add \
     (I18N_m(1008,"Add"))         /*catgets1008*/
#define I18N_Any \
     (I18N_m(1009,"Any"))         /*catgets1009*/
#define I18N_All_Hosts \
     (I18N_m(1010,"All Hosts"))   /*catgets1010*/
#define I18N_Active \
     (I18N_m(1011,"Active"))   /*catgets1011*/
#define I18N_Analyzer \
     (I18N_m(1012,"Analyzer"))   /*catgets1012*/

#define I18N_BEGIN \
     (I18N_m(1200,"BEGIN"))       /*catgets1200*/
#define I18N_Bottom \
     (I18N_m(1201,"Bottom"))      /*catgets1201*/
#define I18N_Browse \
     (I18N_m(1202,"Browse"))      /*catgets1202*/
#define I18N_Brief \
     (I18N_m(1203,"Brief"))       /*catgets1203*/
#define I18N_Backfilling \
     (I18N_m(1204,"Backfilling")) /*catgets1204*/
#define I18N_Boolean \
     (I18N_m(1205,"Boolean"))     /*catgets1205*/
#define I18N_Batch \
     (I18N_m(1206,"Batch"))       /*catgets1206*/
#define I18N_busy \
     (I18N_m(1207,"lock"))       /* catgets 1207*/
#define I18N_minus \
     (I18N_m(1213,"-"))            /*catgets1213*/
#define I18N_semibusy \
     (I18N_m(1214,"-busy"))        /*catgets1214*/

#define I18N_Close \
     (I18N_m(1400,"Close"))       /*catgets1400*/
#define I18N_Contents \
     (I18N_m(1401,"Contents"))    /*catgets1401*/
#define I18N_Checkpoint \
     (I18N_m(1402,"Checkpoint"))  /*catgets1402*/
#define I18N_Command \
     (I18N_m(1403,"Command"))     /*catgets1403*/
#define I18N_Cancel \
     (I18N_m(1404,"Cancel"))      /*catgets1404*/
#define I18N_Choose \
     (I18N_m(1405,"Choose"))      /*catgets1405*/
#define I18N_Clean \
     (I18N_m(1406,"Clean"))       /*catgets1406*/
#define I18N_closed \
     (I18N_m(1407,"closed"))       /*catgets1407*/
#define I18N_Conditions \
     (I18N_m(1408,"Conditions"))   /*catgets1408*/
#define I18N_Calendar \
     (I18N_m(1409,"Calendar"))     /*catgets1409*/
#define I18N_Check \
     (I18N_m(1410,"Check"))        /*catgets1410*/
#define I18N_Commit \
     (I18N_m(1411,"Commit"))       /*catgets1411*/
#define I18N_Clear \
     (I18N_m(1412,"Clear"))        /*catgets1412*/
#define I18N_Checkpointable \
     (I18N_m(1413,"Checkpointable"))  /*catgets1413*/
#define I18N_Cluster \
     (I18N_m(1414,"Cluster"))      /*catgets1414*/
#define I18N_Client \
     (I18N_m(1415,"Client"))       /*catgets1415*/
#define I18N_CPU_Limit \
     (I18N_m(1416,"CPU Limit"))    /*catgets1416*/
#define I18N_Core_Limit \
     (I18N_m(1417,"Core Limit"))   /*catgets1417*/
#define I18N_Copy_From \
     (I18N_m(1418,"Copy From"))    /*catgets1418*/
#define I18N_Configure \
     (I18N_m(1419,"Configure"))    /*catgets1419*/
#define I18N_characters \
     (I18N_m(1420,"characters"))   /*catgets1420*/
#define I18N_CPUF \
     (I18N_m(1421,"CPUF"))         /*catgets1421*/
#define I18N_Closed \
     (I18N_m(1422,"Closed"))         /*catgets1422*/
#define I18N_client \
     (I18N_m(1423,"client"))       /*catgets1423*/
#define I18N_SAAP \
     (I18N_m(1424,"Share group charged")) /*catgets1424*/
#define I18N_JOB_DESC \
     (I18N_m(1425,"Job Description")) /*catgets1425*/

#define I18N_Done \
     (I18N_m(1600,"Done"))        /*catgets1600*/
#define I18N_DISABLED \
     (I18N_m( 1601, "DISABLED"))  /*catgets1601*/
#define I18N_Detail \
     (I18N_m( 1602, "Detail"))    /*catgets1602*/
#define I18N_DONE \
     (I18N_m(1603, "DONE"))       /*catgets1603*/
#define I18N_Delete \
     (I18N_m(1604, "Delete"))     /*catgets1604*/
#define I18N_Default \
     (I18N_m(1605, "Default"))    /*catgets1605*/
#define I18N_Day \
     (I18N_m(1606, "Day"))        /*catgets1606*/
#define I18N_Duration \
     (I18N_m(1607, "Duration"))   /*catgets1607*/
#define I18N_Dependency \
     (I18N_m(1608, "Dependency")) /*catgets1608*/
#define I18N_Defaults \
     (I18N_m(1609, "Defaults"))   /*catgets1609*/
#define I18N_Description \
     (I18N_m(1610, "Description")) /*catgets1610*/
#define I18N_Data_Limit \
     (I18N_m(1611, "Data Limit"))  /*catgets1611*/
#define I18N_Dynamic \
     (I18N_m(1612, "Dynamic"))     /*catgets1612*/
#define I18N_Decreasing \
     (I18N_m(1613, "Decreasing"))  /*catgets1613*/
#define I18N_Directories \
     (I18N_m(1614, "Directories")) /*catgets1614*/
#define I18N_done \
     (I18N_m(1615, "done"))	   /*catgets1615*/

#define I18N_Exiting \
     (I18N_m(1800,"Exiting"))     /*catgets1800*/
#define I18N_Exited \
     (I18N_m(1801,"Exited"))      /*catgets1801*/
#define I18N_error \
     (I18N_m(1802,"error"))       /*catgets1802*/
#define I18N_ENABLED \
     (I18N_m(1803,"ENABLED"))     /*catgets1803*/
#define I18N_END \
     (I18N_m(1804,"END"))         /*catgets1804*/
#define I18N_Exit \
     (I18N_m(1805,"Exit"))        /*catgets1805*/
#define I18N_EXIT \
     (I18N_m(1806,"EXIT"))        /*catgets1806*/
#define I18N_Exec__Host \
     (I18N_m(1807,"Exec_Host"))   /*catgets1807*/
#define I18N_EVENT \
     (I18N_m(1808,"EVENT"))       /*catgets1808*/
#define I18N_except \
     (I18N_m(1809,"except"))       /*catgets1809*/
#define I18N_exit \
     (I18N_m(1810,"exit"))        /*catgets1810*/
#define I18N_EVENTS \
     (I18N_m(1811,"EVENTS"))      /*catgets1811*/
#define I18N_EXPIRED \
     (I18N_m(1812,"EXPIRED"))     /*catgets1812*/
#define I18N_Edit \
     (I18N_m(1813,"Edit"))        /*catgets1813*/
#define I18N_Error_file \
     (I18N_m(1814,"Error file"))  /*catgets1814*/
#define I18N_Exclusive_Job \
     (I18N_m(1815,"Exclusive Job"))   /*catgets1815*/
#define I18N_Event \
     (I18N_m(1816,"Event"))       /*catgets1816*/
#define I18N_Exception \
     (I18N_m(1817,"Exception"))   /*catgets1817*/

#ifdef MOTOROLA_XLSBATCH_ENH
#define I18N_Ext__Msg \
     (I18N_m(1818,"External Message"))    /*catgets1818*/
#define I18N_Err__Option \
     (I18N_m(1819,"Please select at least one job information source!"))    /*catgets1819*/
#define I18N_Err__Days \
     (I18N_m(1820,"Number of day entered is not valid!\nPlease enter a non-negative integer."))    /*catgets1820*/
#endif


#define I18N_From \
     (I18N_m(2000,"From"))        /*catgets2000*/
#define I18N_File \
     (I18N_m(2001,"File"))        /*catgets2001*/
#define I18N_Filter \
     (I18N_m(2002,"Filter"))      /*catgets2002*/
#define I18N_From__Host \
     (I18N_m(2003,"From_Host"))   /*catgets2003*/
#define I18N_Finish__Time \
     (I18N_m(2004,"Finish_Time")) /*catgets2004*/
#define I18N_file \
     (I18N_m(2005,"file"))        /*catgets2005*/
#define I18N_From_file \
     (I18N_m(2006,"From file"))   /*catgets2006*/
#define I18N_File_Limit \
     (I18N_m(2007,"File Limit"))  /*catgets2007*/
#define I18N_Fairshare \
     (I18N_m(2008,"Fairshare"))   /*catgets2008*/
#define I18N_Files \
     (I18N_m(2009,"Files"))       /*catgets2009*/

#define I18N_Group_Name \
     (I18N_m(2200, "Group Name"))   /*catgets2200*/
#define I18N_Go_To_Directory \
     (I18N_m(2201, "Go To Directory")) /*catgets2201*/

#define I18N_Help \
     (I18N_m(2400,"Help"))        /*catgets2400*/
#define I18N_History \
     (I18N_m(2401,"History"))     /*catgets2401*/
#define I18N_Host \
     (I18N_m(2402,"Host"))        /*catgets2402*/
#define I18N_HOSTS \
     (I18N_m(2403,"HOSTS"))       /*catgets2403*/
#define I18N_Hour \
     (I18N_m(2404,"Hour"))        /*catgets2404*/
#define I18N_Hosts \
     (I18N_m(2405,"Hosts"))       /*catgets2405*/
#define I18N_Hold \
     (I18N_m(2406,"Hold"))        /*catgets2406*/
#define I18N_hours \
     (I18N_m(2407,"hours"))       /*catgets2407*/
#define I18N_Host_Types \
     (I18N_m(2408,"Host Types"))  /*catgets2408*/
#define I18N_Host_Models \
     (I18N_m(2409,"Host Models")) /*catgets2409*/
#define I18N_Host_Names \
     (I18N_m(2410,"Host Names"))  /*catgets2410*/
#define I18N_Host_Spec \
     (I18N_m(2411,"Host Spec"))   /*catgets2411*/
#define I18N_Host_Type \
     (I18N_m(2412,"Host Type"))   /*catgets2412*/
#define I18N_Host_Model \
     (I18N_m(2413,"Host Model"))  /*catgets2413*/
#define I18N_Host_Name \
     (I18N_m(2414,"Host Name"))   /*catgets2414*/
#define I18N_Host_Group \
     (I18N_m(2415,"Host Group"))  /*catgets2415*/
#define I18N_Host_Partition \
     (I18N_m(2416,"Host Partition"))  /*catgets2416*/
#define I18N_HOST \
     (I18N_m(2417,"HOST"))       /*catgets2417*/
#define I18N_Host__Name \
     (I18N_m(2418,"Host_Name"))   /*catgets2418*/

#define I18N_info \
     (I18N_m(2600, "info"))       /*catgets2600*/
#define I18N_Information \
     (I18N_m(2601, "Information"))/*catgets2601*/
#define I18N_inactivated \
     (I18N_m(2602, "inactivated"))/*catgets2602*/
#define I18N_inactive \
     (I18N_m(2603, "inactive"))   /*catgets2603*/
#define I18N_invalid \
     (I18N_m(2604, "invalid"))    /*catgets2604*/
#define I18N_Insert \
     (I18N_m(2605, "Insert"))     /*catgets2605*/
#define I18N_ID \
     (I18N_m(2606, "ID"))         /*catgets2606*/
#define I18N_Info \
     (I18N_m(2607, "Info"))       /*catgets2607*/
#define I18N_Input_file \
     (I18N_m(2608, "Input file")) /*catgets2608*/
#define I18N_Increasing \
     (I18N_m(2609, "Increasing")) /*catgets2609*/
#define I18N_Interactive \
     (I18N_m(2610, "Interactive")) /*catgets2610*/
#define I18N_Inactive \
     (I18N_m(2611, "Inactive"))   /*catgets2611*/
#define I18N_Inact \
     (I18N_m(2612, "Inact"))      /*catgets2612*/
#define I18N_Inact__Adm \
     (I18N_m(2613, "Inact_Adm"))  /*catgets2613*/
#define I18N_Inact__Win \
     (I18N_m(2614, "Inact_Win"))  /*catgets2614*/

#define I18N_Job \
     (I18N_m(2800, "Job"))        /*catgets2800*/
#define I18N_JobArray \
     (I18N_m(2801, "JobArray"))   /*catgets2801*/
#define I18N_Job__Id \
     (I18N_m(2802, "Job_Id"))     /*catgets2802*/
#define I18N_Job__PID \
     (I18N_m(2803, "Job_PID"))    /*catgets2803*/
#define I18N_Job__Name \
     (I18N_m(2804, "Job_Name"))   /*catgets2804*/
#define I18N_JobGroup \
     (I18N_m(2805, "Job Group"))  /*catgets2805*/
#define I18N_Job_ID \
     (I18N_m(2806, "Job ID"))     /*catgets2806*/
#define I18N_Job_Name \
     (I18N_m(2807, "Job Name"))   /*catgets2807*/
#define I18N_Job_Dependency \
     (I18N_m(2808, "Job Dependency"))  /*catgets2808*/
#define I18N_JL_U \
     (I18N_m(2809, "JL/U"))       /*catgets2809*/
#define I18N_JL_P \
     (I18N_m(2810, "JL/P"))       /*catgets2810*/
#define I18N_JL_H \
     (I18N_m(2811, "JL/H"))       /*catgets2811*/
#define I18N_JOBID \
     (I18N_m(2812, "JOBID"))     /*catgets2812*/
#define I18N_JOB__NAME \
     (I18N_m(2813, "JOB_NAME"))   /*catgets2813*/
#define I18N_Job_Priority \
     (I18N_m(2814,"Job Priority"))         /*catgets2814*/
#define I18N_JobScheduler \
     (I18N_m(2815, "JobScheduler"))   /*catgets2815*/
#ifdef MOTOROLA_XLSBATCH_ENH
#define I18N_Job__Source \
     (I18N_m(2816,"Job Information Source"))         /*catgets2816*/
#define I18N_Job__Source_Current \
     (I18N_m(2817,"Current"))         /*catgets2817*/
#define I18N_Job__Source_History \
     (I18N_m(2818,"Historical"))         /*catgets2818*/
#define I18N_Job__Source_Label \
     (I18N_m(2819,"Show jobs for the last"))       /*catgets2819*/
#define I18N_Job__Source_Days \
     (I18N_m(2820,"days"))       /*catgets2820*/
#endif

#define I18N_killed \
     (I18N_m(3000, "killed"))     /*catgets3000*/
#define I18N_KBytes \
     (I18N_m(3001, "KBytes"))     /*catgets3001*/

#define I18N_Limits \
     (I18N_m(3200,"Limits"))      /*catgets3200*/
#define I18N_Load \
     (I18N_m(3201,"Load"))        /*catgets3201*/
#define I18N_LSF_Batch \
     (I18N_m(3202,"LSF Batch"))   /*catgets3202*/
#define I18N_LSF_Administrators \
     (I18N_m(3203,"LSF Administrators"))   /*catgets3203*/
#define I18N_Local \
     (I18N_m(3204,"Local"))       /*catgets3204*/
#define I18N_Load_Files \
     (I18N_m(3205,"Load Files"))       /*catgets3205*/
#define I18N_lock \
     (I18N_m(3206,"lock"))        /*catgets3206*/
#define I18N_lockUW \
     (I18N_m(3207,"-lockUW"))      /*catgets3207*/
#define I18N_lockU \
     (I18N_m(3208,"-lockU"))      /*catgets3208*/
#define I18N_lockW \
     (I18N_m(3209,"-lockW"))      /*catgets3209*/
#define I18N_lockM \
     (I18N_m(3211,"-lockM"))      /*catgets3211*/
#define I18N_lockUM \
     (I18N_m(3212,"-lockUM"))      /*catgets3212*/
#define I18N_lockWM \
     (I18N_m(3213,"-lockWM"))      /*catgets3213*/
#define I18N_lockUWM \
     (I18N_m(3214,"-lockUWM"))      /*catgets3214*/
#define I18N_LSF \
     (I18N_m(3210,"LSF"))        /*catgets3210*/

#define I18N_Mail \
     (I18N_m(3400,"Mail"))        /*catgets3400*/
#define I18N_more \
     (I18N_m(3401,"more"))        /*catgets3401*/
#define I18N_min \
     (I18N_m(3402,"min"))         /*catgets3402*/
#define I18N_Manipulate \
     (I18N_m(3403,"Manipulate"))  /*catgets3403*/
#define I18N_Migrate \
     (I18N_m(3404,"Migrate"))     /*catgets3404*/
#define I18N_Modify \
     (I18N_m(3405,"Modify"))      /*catgets3405*/
#define I18N_Month \
     (I18N_m(3406,"Month"))       /*catgets3406*/
#define I18N_Min \
     (I18N_m(3407,"Min"))         /*catgets3407*/
#define I18N_minutes \
     (I18N_m(3408,"minutes"))     /*catgets3408*/
#define I18N_Max \
     (I18N_m(3409,"Max"))         /*catgets3409*/
#define I18N_Minutes \
     (I18N_m(3410,"Minutes"))     /*catgets3410*/
#define I18N_Memory_Limit \
     (I18N_m(3411,"Memory Limit"))  /*catgets3411*/
#define I18N_Manage \
     (I18N_m(3412,"Manage")) 	  /*catgets3412*/
#define I18N_More \
     (I18N_m(3413,"More"))        /*catgets3413*/
#define I18N_MAX \
     (I18N_m(3414,"MAX"))         /*catgets3414*/

#define I18N_Name \
     (I18N_m(3600,"Name"))        /*catgets3600*/
#define I18N_NONE \
     (I18N_m(3601,"NONE"))        /*catgets3601*/
#define I18N_No \
     (I18N_m(3602,"No"))          /*catgets3602*/
#define I18N_NJOBS \
     (I18N_m(3603,"NJOBS"))       /*catgets3603*/
#define I18N_None \
     (I18N_m(3604,"None"))        /*catgets3604*/
#define I18N_NAME \
     (I18N_m(3605,"NAME"))        /*catgets3605*/
#define I18N_New \
     (I18N_m(3606,"New"))         /*catgets3606*/
#define I18N_Next \
     (I18N_m(3607,"Next"))        /*catgets3607*/
#define I18N_NICE \
     (I18N_m(3608,"NICE"))        /*catgets3608*/
#define I18N_no_limit \
     (I18N_m(3609,"no limit"))    /*catgets3609*/
#define I18N_Numeric \
     (I18N_m(3610,"Numeric"))     /*catgets3610*/
#define I18N_Nonrelease \
     (I18N_m(3611,"Nonrelease"))  /*catgets3611*/


#define I18N_OK \
     (I18N_m(3800,"OK"))          /*catgets3800*/
#define I18N_or \
     (I18N_m(3801,"or"))          /*catgets3801*/
#define I18N_OWNER \
     (I18N_m(3802,"OWNER"))       /*catgets3802*/
#define I18N_Options \
     (I18N_m(3803,"Options"))     /*catgets3803*/
#define I18N_ok \
     (I18N_m(3804,"ok"))          /*catgets3804*/
#define I18N_opened \
     (I18N_m(3805,"opened"))      /*catgets3805*/
#define I18N_OPEN \
     (I18N_m(3806,"OPEN"))        /*catgets3806*/
#define I18N_Output_file \
     (I18N_m(3807,"Output file")) /*catgets3807*/
#define I18N_Only \
     (I18N_m(3808,"Only"))        /*catgets3808*/
#define I18N_open \
     (I18N_m(3809,"open"))        /*catgets3809*/
#define I18N_Open \
     (I18N_m(3810,"Open"))        /*catgets3810*/
#define I18N_semiok \
     (I18N_m(3811,"-ok"))         /*catgets3811*/

#define I18N_Parameter \
     (I18N_m(4000,"Parameter"))   /*catgets4000*/
#define I18N_Peek \
     (I18N_m(4001,"Peek"))        /*catgets4001*/
#define I18N_Print \
     (I18N_m(4002,"Print"))       /*catgets4002*/
#define I18N_PEND \
     (I18N_m(4003,"PEND"))        /*catgets4003*/
#define I18N_PSUSP \
     (I18N_m(4004,"PSUSP"))       /*catgets4004*/
#define I18N_Previous \
     (I18N_m(4005,"Previous"))    /*catgets4005*/
#define I18N_Priority \
     (I18N_m(4006,"Priority"))    /*catgets4006*/
#define I18N_Processor_Limit \
     (I18N_m(4009,"Processor Limit")) /*catgets4009*/
#define I18N_parameters \
     (I18N_m(4010,"parameters"))  /*catgets4010*/
#define I18N_PRIO \
     (I18N_m(4011,"PRIO"))       /*catgets4011*/
#define I18N_Process_Limit \
     (I18N_m(4012,"Process Limit")) /*catgets4012*/
#define I18N_pending \
     (I18N_m(4013,"pending"))      /*catgets4013*/
     
#define I18N_Queue \
     (I18N_m(4200,"Queue"))       /*catgets4200*/
#define I18N_QUEUE \
     (I18N_m(4201,"QUEUE"))       /*catgets4201*/
#define I18N_Queue__Name \
     (I18N_m(4202,"Queue_Name"))  /*catgets4202*/
#define I18N_QUEUE__NAME \
     (I18N_m(4203,"QUEUE_NAME"))  /*catgets4203*/

#define I18N_Request \
     (I18N_m(4400,"Request"))     /*catgets4400*/
#define I18N_resumed \
     (I18N_m(4401,"resumed"))     /*catgets4401*/
#define I18N_RUNNING \
     (I18N_m(4402,"RUNNING"))     /*catgets4402*/
#define I18N_Resume \
     (I18N_m(4403,"Resume"))      /*catgets4403*/
#define I18N_RUN \
     (I18N_m(4404,"RUN"))         /*catgets4404*/
#define I18N_Restart \
     (I18N_m(4405,"Restart"))     /*catgets4405*/
#define I18N_Revert \
     (I18N_m(4406,"Revert"))      /*catgets4406*/
#define I18N_RESOLVED \
     (I18N_m(4407,"RESOLVED"))    /*catgets4407*/
#define I18N_Resourses \
     (I18N_m(4408,"Resources"))   /*catgets4408*/
#define I18N_Remove \
     (I18N_m(4409,"Remove"))      /*catgets4409*/
#define I18N_Replace \
     (I18N_m(4410,"Replace"))     /*catgets4410*/
#define I18N_Rerunable_Job \
     (I18N_m(4411,"Rerunable Job"))   /*catgets4411*/
#define I18N_Resources \
     (I18N_m(4412,"Resources"))   /*catgets4412*/
#define I18N_Run_Limit \
     (I18N_m(4413,"Run Limit"))   /*catgets4413*/
#define I18N_Rerunable \
     (I18N_m(4414,"Rerunable"))   /*catgets4414*/
#define I18N_Resource \
     (I18N_m(4415,"Resource"))    /*catgets4415*/
#define I18N_rename \
     (I18N_m(4416,"rename"))      /*catgets4416*/
#define I18N_Remote \
     (I18N_m(4417,"Remote"))      /*catgets4417*/
#define I18N_RSV \
     (I18N_m(4418,"RSV"))         /*catgets4418*/
#define I18N_running \
     (I18N_m(4419,"running"))         /*catgets4419*/
#define I18N_SUSPENDED \
     (I18N_m(4420,"SUSPENDED"))         /*catgets4420*/
#define I18N_Release \
     (I18N_m(4421, "Release"))          /*catgets4421*/

#define I18N_sec \
     (I18N_m(4600,"sec"))         /*catgets4600*/
#define I18N_signaled \
     (I18N_m(4601,"signaled"))    /*catgets4601*/
#define I18N_Sorry \
     (I18N_m(4602,"Sorry"))       /*catgets4602*/
#define I18N_stopped \
     (I18N_m(4603,"stopped"))     /*catgets4603*/
#define I18N_Signal \
     (I18N_m(4604,"Signal"))      /*catgets4604*/
#define I18N_Sort \
     (I18N_m(4605,"Sort"))        /*catgets4605*/
#define I18N_Suspend \
     (I18N_m(4606,"Suspend"))     /*catgets4606*/
#define I18N_Switch \
     (I18N_m(4607,"Switch"))      /*catgets4607*/
#define I18N_SSUSP \
     (I18N_m(4608,"SSUSP"))       /*catgets4608*/
#define I18N_Submit \
     (I18N_m(4609,"Submit"))      /*catgets4609*/
#define I18N_Summary \
     (I18N_m(4610,"Summary"))     /*catgets4610*/
#define I18N_Stat \
     (I18N_m(4611,"Stat"))        /*catgets4611*/
#define I18N_Sub__Time \
     (I18N_m(4612,"Sub_Time"))    /*catgets4612*/
#define I18N_Start__Time \
     (I18N_m(4613,"Start_Time"))  /*catgets4613*/
#define I18N_STATUS \
     (I18N_m(4614,"STATUS"))      /* catgets4614 */
#define I18N_Select \
     (I18N_m(4615,"Select"))      /*catgets4615*/
#define I18N_seconds \
     (I18N_m(4616,"seconds"))     /*catgets4616*/
#define I18N_SOURCE \
     (I18N_m(4617,"SOURCE"))      /*catgets4617*/
#define I18N_SEVERITY \
     (I18N_m(4618,"SEVERITY"))    /*catgets4618*/
#define I18N_standard \
     (I18N_m(4619,"standard"))    /*catgets4619*/
#define I18N_system \
     (I18N_m(4620,"system"))      /*catgets4620*/
#define I18N_Save \
     (I18N_m(4621,"Save"))        /*catgets4621*/
#define I18N_Save__As \
     (I18N_m(4622,"Save_As"))     /*catgets4622*/
#define I18N_Save_As \
     (I18N_m(4623,"Save As"))     /*catgets4623*/
#define I18N_Shared \
     (I18N_m(4624,"Shared"))      /*catgets4624*/
#define I18N_Server \
     (I18N_m(4625,"Server"))      /*catgets4625*/
#define I18N_Stack_Limit \
     (I18N_m(4626,"Stack Limit")) /*catgets4626*/
#define I18N_Swap_Limit \
     (I18N_m(4627,"Swap Limit"))  /*catgets4627*/
#define I18N_Static \
     (I18N_m(4628,"Static"))      /*catgets4628*/
#define I18N_String \
     (I18N_m(4629,"String"))      /*catgets4629*/
#define I18N_Some_Hosts \
     (I18N_m(4630,"Some Hosts"))  /*catgets4630*/
#define I18N_Shared_by \
     (I18N_m(4631,"Shared by:"))  /*catgets4631*/
#define I18N_Selection \
     (I18N_m(4632,"Selection"))   /*catgets4632*/
#define I18N_Save_Files \
     (I18N_m(4633,"Save Files"))   /*catgets4633*/
#define I18N_Select_File \
     (I18N_m(4634,"Select File"))  /*catgets4634*/
#define I18N_Status \
     (I18N_m(4635,"Status"))       /*catgets4635 */
#define I18N_SUSP \
     (I18N_m(4636,"SUSP"))         /*catgets4636*/
#define I18N_suspended \
     (I18N_m(4637,"suspended"))         /*catgets4637*/

#define I18N_To \
     (I18N_m(4800,"To"))          /*catgets4800*/
#define I18N_terminated \
     (I18N_m(4801,"terminated"))  /*catgets4801*/
#define I18N_Terminate \
     (I18N_m(4802,"Terminate"))   /*catgets4802*/
#define I18N_KillRequeue \
     (I18N_m(4806,"Kill & Requeue"))   /*catgets4806*/
#define I18N_KillRemove \
     (I18N_m(4806,"Kill & Remove"))   /*catgets4807*/
#define I18N_Top \
     (I18N_m(4803,"Top"))         /*catgets4803*/
#define I18N_Task \
     (I18N_m(4804,"Task"))        /*catgets4804*/
#define I18N_TOTAL \
     (I18N_m(4805,"TOTAL"))       /*catgets4805*/

#define I18N_Usage \
     (I18N_m(5000,"Usage"))       /*catgets5000*/
#define I18N_undefined \
     (I18N_m(5001,"undefined"))   /*catgets5001*/
#define I18N_UNKNOWN \
     (I18N_m(5002,"UNKNOWN"))     /*catgets5002*/
#define I18N_Update \
     (I18N_m(5003,"Update"))      /*catgets5003*/
#define I18N_Updated \
     (I18N_m(5004,"Updated"))     /*catgets5004*/
#define I18N_User \
     (I18N_m(5005,"User"))        /*catgets5005*/
#define I18N_USUSP \
     (I18N_m(5006,"USUSP"))       /*catgets5006*/
#define I18N_unavail \
     (I18N_m(5007,"unavail"))     /*catgets5007*/
#define I18N_USERS \
     (I18N_m(5009, "USERS"))      /*catgets5009*/
#define I18N_user \
     (I18N_m(5010, "user"))       /*catgets5010*/
#define I18N_User_Group \
     (I18N_m(5010, "User Group"))  /*catgets5010*/
#define I18N_Users \
     (I18N_m(5011,"Users"))        /*catgets5011*/
#define I18N_USER \
     (I18N_m(5012, "USER"))        /*catgets5012*/
#define I18N_USER_GROUP \
     (I18N_m(5013, "USER/GROUP"))  /*catgets5013*/
#define I18N_unknown \
     (I18N_m(5014, "unknown"))  /*catgets5014*/
#define I18N_unreach \
     (I18N_m(5015, "unreach"))  /*catgets5015*/
#define I18N_User_Priority \
     (I18N_m(5016,"User Priority"))         /*catgets5016*/
#define I18N_MPJOBS \
     (I18N_m(5017,"MPJOBS"))         /*catgets5017*/
#define I18N_PJOBS \
     (I18N_m(5018,"PJOBS"))         /*catgets5018*/
#define I18N_MPEND \
     (I18N_m(5019,"MPEND"))         /*catgets5019*/
#define I18N_SHARES \
     (I18N_m(5020, "SHARES"))      /*catgets5020*/
#define I18N_STARTED \
     (I18N_m(5021, "STARTED"))      /*catgets5021*/
#define I18N_CPU_TIME \
     (I18N_m(5022, "CPU_TIME"))      /*catgets5022*/
#define I18N_RUN_TIME \
     (I18N_m(5023, "RUN_TIME"))      /*catgets5023*/
#define I18N_PRIORITY \
     (I18N_m(5024, "PRIORITY"))      /*catgets5024*/

#define I18N_View \
     (I18N_m(5200,"View"))        /*catgets5200*/
#define I18N_Value \
     (I18N_m(5201,"Value"))       /*catgets5201*/

#define I18N_Warning \
     (I18N_m(5400,"Warning"))     /*catgets5400*/
#define I18N_WAITING \
     (I18N_m(5401,"WAITING"))     /*catgets5401*/
#define I18N_Windows \
     (I18N_m(5402,"Windows"))     /*catgets5402*/

#define I18N_Yes \
     (I18N_m(5800, "Yes"))        /* catgets5800*/

#define I18N_zombi \
     (I18N_m(6000, "zombi"))        /* catgets6000*/

#undef  NL_SETN 
#define I18N(msgID, msg)\
       (_i18n_msg_get(ls_catd, NL_SETN, msgID, msg))

     
     


#define MAX_I18N_CTIME_STRING		80
#define MIN_CTIME_FORMATID		0
#define CTIME_FORMAT_DEFAULT		0	
#define CTIME_FORMAT_a_b_d_T_Y		1	
#define CTIME_FORMAT_b_d_T_Y		2	
#define CTIME_FORMAT_a_b_d_T		3	
#define CTIME_FORMAT_b_d_H_M		4	
#define CTIME_FORMAT_m_d_Y		5	
#define CTIME_FORMAT_H_M_S		6	
#define MAX_CTIME_FORMATID		6

extern LS_CATD ls_catd;			
extern int _i18n_init(int);		
extern int _i18n_end();			
extern char * _i18n_ctime(LS_CATD, int, const time_t *);

#ifdef  I18N_COMPILE 
extern char * _i18n_msg_get(LS_CATD, int, int,char *);
extern char ** _i18n_msgArray_get(LS_CATD, int, int *, char **);
extern void _i18n_ctime_init(LS_CATD);
#else
#define  _i18n_msg_get(catd,setID,msgID,msgStr)  (msgStr)
#define  _i18n_msgArray_get(catd,setID,msgID_Array,msgArray) (msgArray)
#endif

char * _i18n_ctime(LS_CATD, int, const time_t *);
char * _i18n_printf(const char *, ...);



#endif 

