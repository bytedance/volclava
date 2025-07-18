/* $Id: lib.words.c 397 2007-11-26 19:04:00Z mblack $
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
#include "lib.h"
#include "lproto.h"

char *
getNextLine_(FILE *fp, int confFormat)
{
    int dummy = 0;
    return (getNextLineC_(fp, &dummy, confFormat));
}

char *
getNextWord_(char **line)
{
    static char word[4*MAXLINELEN];
    char *wordp = word;

    while(isspace(**line))
        (*line)++;

    while(**line && !isspace(**line))
        *wordp++ = *(*line)++;

    if (wordp == word)
        return(NULL);

    *wordp = '\0';
    return(word);
}

char *
getNextWord1_(char **line)
{
    static char word[4*MAXLINELEN];
    char *wordp = word;



    while(isspace(**line))
        (*line)++;


    while (**line && !isspace(**line)&& (**line != ',')
           && (**line != ']')&& (**line != '['))
        *wordp++ = *(*line)++;

    if (wordp == word)

        return(NULL);


    *wordp = '\0';
    return(word);

}

static int charInSet(char c, const char *set)
{
    while (*set != '\0') {
        if (c == *set) {
            return TRUE;
        }
        set++;
    }
    return FALSE;
}

char *
getNextWordSet(char **line, const char *set)
{
    static char word[4*MAXLINELEN];
    char *wordp = word;


    while(charInSet(**line, set))
        (*line)++;


    while (**line && !charInSet(**line, set))
        *wordp++ = *(*line)++;

    if (wordp == word)

        return(NULL);


    *wordp = '\0';
    return(word);

}

char *
getNextValueQ_(char **line, char ch1, char ch2)
{
    char *sp, str[4];
    static char *value;
    int valuelen;

    lserrno = LSE_NO_ERR;
    sp = getNextWord_(line);
    if (!sp)
        return NULL;

    if (sp[0] != ch1)
        return sp;




    sprintf(str, "%c", ch1);
    if (strcmp(sp, str) == 0) {
        sp = getNextWord_(line);
        if (sp == NULL) {
            lserrno = LSE_CONF_SYNTAX;
            return (NULL);
        }
    } else
        sp++;


    if (value != NULL)
        free(value);
    if (strlen(sp) > MAXLINELEN) {
        valuelen = 2 * strlen(sp);
        value = malloc(valuelen);
    } else {
        value = malloc(MAXLINELEN);
        valuelen = MAXLINELEN;
    }

    if (value == NULL) {
        lserrno = LSE_MALLOC;
        return (NULL);
    }
    strcpy(value, sp);
    sp = strchr(value, ch2);
    if (sp != NULL) {
        *sp = '\0';
        return value;
    }

    sprintf(str, "%c", ch2);
    while ((sp = getNextWord_(line)) != NULL) {
        if (strcmp(sp, str) == 0)
            return value;

        if (strlen(value)+strlen(sp)+2 > valuelen-1) {
            char *newvp;
            newvp = myrealloc(value, valuelen+strlen(sp)+MAXLINELEN);
            if (newvp == NULL)
                return value;
            value = newvp;
            valuelen += MAXLINELEN;
        }

        strcat(value, " ");
        strcat(value, sp);
        sp = strrchr(value, ch2);
        if (sp != NULL) {
            *sp = '\0';
            return value;
        }
    }


    FREEUP(value);
    lserrno = LSE_CONF_SYNTAX;
    return NULL;

}

int
stripQStr (char *q, char *str)
{
    char *fr = q;

    for (; *q != '"' && *q != '\0'; q++);
    if (*q == '\0')
        return (-1);

    for (q++; *q != '\0'; q++, str++) {
        if (*q == '"') {
            if (*(q+1) == '"')
                q++;
            else {
                *str = '\0';
                break;
            }
        }
        *str = *q;
    }

    if (*q == '\0')
        return (-1);
    return (q-fr+1);
}

int
addQStr (FILE *log_fp, char *str)
{
    int j = 1;

    if (putc (' ', log_fp) == EOF)
        return -1;
    if (putc ('"', log_fp) == EOF)
        return -1;
    for (; *str != '\0'; str++, j++) {
        if (*str == '"')
            if (putc ('"', log_fp) == EOF)
                return -1;
        if (putc (*str, log_fp) == EOF)
            return -1;
    }
    if (putc ('"', log_fp) == EOF)
        return -1;

    return 0;
}

char *
getNextLineD_(FILE *fp, int *LineCount, int confFormat)
{
    static char *line = NULL;
    int cin, lpos, oneChar, cinBslash;
    int linesize = MAXLINELEN;
    int quotes = 0;

    lserrno = LSE_NO_ERR;

    oneChar = -1;

    if (line != NULL)
        FREEUP (line);

    line = calloc(1, MAXLINELEN);
    if (line == NULL) {
        lserrno = LSE_MALLOC;
        return (NULL);
    }


        lpos = 0;
        while ((cin = getc(fp)) != EOF) {

            if (cin == '\n') {
                *LineCount += 1;
                break;
            }
            if (cin == '"') {
                if (quotes == 0)
                    quotes++;
                else
                    quotes--;
            }
            if (confFormat && cin == '#' && quotes == 0) {

                while ((cin = getc(fp)) != EOF)
                    if (cin == '\n') {
                        *LineCount += 1;
                        break;
                    }
                break;
            }


            if (confFormat && cin == '\\') {
                cinBslash = cin;
                if ((cin = getc(fp)) == EOF)
                    break;

                if (cin == '\n')
                    *LineCount += 1;
                else if (!isspace(cin)) {

                    if (lpos < linesize - 1)
                        line[lpos++] = cinBslash;
                    else {
                        char *sp;
                        linesize += MAXLINELEN;
                        sp = myrealloc(line, linesize);
                        if (sp == NULL)
                            break;
                        line = sp;
                        line[lpos++] = cinBslash;
                    }
                }
            }


            if (isspace(cin))
                cin = ' ';


            if (lpos < linesize - 1)
                line[lpos++] = cin;
            else {
                char *sp;
                linesize += MAXLINELEN;
                sp = myrealloc(line, linesize);
                if (sp == NULL)
                    break;
                line = sp;
                line[lpos++] = cin;
            }
        }

        if (lpos == 1)
            oneChar = 1;


        while(lpos > 0 && (line[--lpos] == ' '))
            ;

    if ((cin != EOF) || (oneChar == 1) || (cin == EOF && lpos > 0)) {

        line[++lpos] = '\0';
        return(line);
    }

    return(NULL);
}

char *
getNextLineC_(FILE *fp, int *LineCount, int confFormat)
{
    char *nextLine;
    char *sp;

    nextLine = getNextLineD_(fp, LineCount, confFormat);

    if (nextLine == NULL)
        return NULL;



    for (sp=nextLine; *sp != '\0'; sp++)
        if (*sp != ' ')
            return (nextLine);


    return (getNextLineC_(fp, LineCount, confFormat));

}


void
subNewLine_(char* instr) {
    int i, k, strlength;

    if ( instr && (strlength = strlen(instr))) {
        for ( i = strlength-1; i >= 0; i--) {
            if( instr[i] == '\n' ) {
                for( k = i; k < strlength; k++ ) {
                    instr[k] = instr[k+1];
                }
            }
        }
    }
}

/* Get the next meaningful line from the configuration
 * file, meaningful is a line that is not isspace()
 * and that does not start with a comment # character.
 */
char *
nextline_(FILE *fp)
{
    static char  buf[BUFSIZ];
    char         *p;

    p = NULL;
    while (fgets(buf, BUFSIZ, fp)) {
        p = buf;
        while (isspace(*p))
            ++p;
        if (*p == '#'
            || *p == 0) {
	    /* If this is the last or only
	     * line do not return the
	     * previous buffer to the caller.
	     */
	    p = NULL;
            continue;
	}
        break;
    }

    return(p);

} /* nextline_() */
