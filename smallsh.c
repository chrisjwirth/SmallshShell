#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <stddef.h>
#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>

void exitProgram(char *statusChar); 
char *expandWord(char *word);
char *getEnvOrDefaultVal(char *env, char *default_val);
char *strSub(char *restrict *restrict haystack, char const *restrict needle, char const *restrict sub);

pid_t fgPid = 0,
      bgPid = 0,
      lastBgPid = 0,
      spawnPid = -5;

int fgStatus, 
    bgStatus;

int main(void) {
  struct sigaction oldHandler = {0}, 
                   defaultAction = {0}, 
                   ignoreAction = {0};
  defaultAction.sa_handler = SIG_DFL;
  ignoreAction.sa_handler = SIG_IGN;
  sigaction(SIGTSTP, &ignoreAction, &oldHandler);
  sigaction(SIGINT, &ignoreAction, &oldHandler);

  char *line = NULL;
  size_t n = 0;

_MainLoop:
  while (true) {
    
    // Manage background processes
    while ((bgPid = waitpid(0, &bgStatus, WUNTRACED | WNOHANG)) > 0) {
      if (WIFEXITED(bgStatus)) {
        fprintf(stderr, "Child process %jd done. Exit status %d.\n", (intmax_t) bgPid, WEXITSTATUS(bgStatus));
      } else if (WIFSIGNALED(bgStatus)) {
        fprintf(stderr, "Child process %jd done. Signaled %d.\n", (intmax_t) bgPid, WTERMSIG(bgStatus));
      } else if (WIFSTOPPED(bgStatus)) {
        kill(bgPid, SIGCONT);
        fprintf(stderr, "Child process %jd stopped. Continuing.\n", (intmax_t) bgPid);
      }
    }

    // Prompt
    fprintf(stderr, "%s", getEnvOrDefaultVal("PS1", ""));

    // Get line
    sigaction(SIGINT, &defaultAction, NULL);
    ssize_t lineLength = getline(&line, &n, stdin);
    sigaction(SIGINT, &ignoreAction, NULL);
    if (feof(stdin)) exitProgram(expandWord(strdup("$?")));
    if (lineLength == -1 || (strcmp(line, "\n") == 0)) {
      clearerr(stdin);
      goto _MainLoop;
    }

    // Create array to store pointer of expanded copies of each word
    char *wordList[512] = {NULL};
    size_t wordListLen = sizeof wordList / sizeof wordList[0];

    for (size_t i = 0; i < wordListLen; i++) {
      // Tokenize line
      char *token = NULL;
      char *delim = getEnvOrDefaultVal("IFS", " \t\n");    
      if (i == 0) {
        token = strtok(line, delim);
      } else {
        token = strtok(NULL, delim);
      }

      if (token == NULL) break;
      char *tokenCopy = strdup(token);

      // Save expanded copy of each word
      char *expandedWord = expandWord(tokenCopy);
      wordList[i] = expandedWord;
    }

    // Parse word list
    bool executeInBackground = false;
    char *inputRedirectFilename = NULL,
         *outputRedirectFilename = NULL;
    int inputRedirectFilenameIdx = -1,
        outputRedirectFilenameIdx = -1,
        inOutRedirectIdx = -1,
        lastRegularArgumentIdx = 0;
    
    for (size_t i = 0; i < wordListLen; i++) {
      // look for end of wordList or a comment
      if (wordList[i] == NULL || (strcmp(wordList[i], "#") == 0)) {

        if (i == 0) break;
        
        // look for background execution flag
        if (strcmp(wordList[i - 1], "&") == 0) {
          lastRegularArgumentIdx = i - 2;
          executeInBackground = true;
          inOutRedirectIdx = i - 3;
        } else {
          inOutRedirectIdx = i - 2;
        }

        if (inOutRedirectIdx < 0) break;


        // look for input and output redirect operators
        if (strcmp(wordList[inOutRedirectIdx], "<") == 0) {
          inputRedirectFilenameIdx = inOutRedirectIdx + 1;
          inOutRedirectIdx -= 2;
          if (inOutRedirectIdx < 0) break;
          if (strcmp(wordList[inOutRedirectIdx], ">") == 0) outputRedirectFilenameIdx = inOutRedirectIdx + 1;
        } else if (strcmp(wordList[inOutRedirectIdx], ">") == 0) {
          outputRedirectFilenameIdx = inOutRedirectIdx + 1;
          inOutRedirectIdx -= 2;
          if (inOutRedirectIdx < 0) break;
          if (strcmp(wordList[inOutRedirectIdx], "<") == 0) inputRedirectFilenameIdx = inOutRedirectIdx + 1;
        }

        if (inputRedirectFilenameIdx != -1) {
          inputRedirectFilename = malloc(strlen(wordList[inputRedirectFilenameIdx]) + 1);
          strcpy(inputRedirectFilename, wordList[inputRedirectFilenameIdx]);
        }

        if (outputRedirectFilenameIdx != -1) {
          outputRedirectFilename = malloc(strlen(wordList[outputRedirectFilenameIdx]) + 1);
          strcpy(outputRedirectFilename, wordList[outputRedirectFilenameIdx]);
        }

        if (inputRedirectFilename && outputRedirectFilename) {
          lastRegularArgumentIdx = inOutRedirectIdx - 1;
        } else {
          lastRegularArgumentIdx = inOutRedirectIdx + 1;
        }

        break;
      }
    }

    if (lastRegularArgumentIdx == -1) goto _endPrompt;

    // Execution
    char *command = wordList[0];
    
    // Built-in exit command
    if (strcmp(command, "exit") == 0) {
      
      char *statusChar = NULL;
      if (lastRegularArgumentIdx == 1) {
        statusChar = wordList[lastRegularArgumentIdx];
      } else if (lastRegularArgumentIdx == 0) {
        statusChar = expandWord(strdup("$?"));
      } else {
        err(-1, "Invalid argument provided to built-in exit command.");
      }

      exitProgram(statusChar);
    }

    // Built-in cd command
    if (strcmp(command, "cd") == 0) {

      char *dir = NULL;
      if (lastRegularArgumentIdx == 1) {
        dir = wordList[lastRegularArgumentIdx];
      } else if (lastRegularArgumentIdx == 0) {
        dir = expandWord(strdup("~/"));
      } else {
        err(-1, "Invalid argument provided to built-in cd command.");
      }

      chdir(dir);
      goto _endPrompt;
    }

    // Non-built-in commands
    spawnPid = fork();
    switch (spawnPid) {
      case -1: {  // Failed fork
        err(-1, "fork() failed!");
        exit(1);
        break;
      } case 0: {  // Child process
          sigaction(SIGINT, &oldHandler, NULL);
          sigaction(SIGTSTP, &oldHandler, NULL);
          
          size_t numArgs = lastRegularArgumentIdx;
          char *args[numArgs];
          args[0] = command;
          for (size_t i = 1; i <= numArgs; i++) args[i] = wordList[i];
          args[numArgs+1] = NULL;

          // Open input file
          // TODO: Error if file does not exist of can't be opened
          if (inputRedirectFilename) {
            int fdIn = open(inputRedirectFilename, O_RDONLY);
            dup2(fdIn, STDIN_FILENO);
            close(fdIn);
          }

          if (outputRedirectFilename) {
            int fdOut = open(outputRedirectFilename, O_RDWR | O_CREAT | O_CLOEXEC, 0777);
            dup2(fdOut, STDOUT_FILENO);
            close(fdOut);
          }

          if (execvp(command, args) == -1) err(errno, "Failed to execvp command.");

          break;
      } default: {  // Parent process
        if (executeInBackground) {
          lastBgPid = spawnPid;
        } else {
          fgPid = waitpid(spawnPid, &fgStatus, 0);
          if (WIFSTOPPED(fgStatus)) {
            if (kill(fgPid, SIGCONT) == -1) err(errno, "Failed to continue child process.");
            fprintf(stderr, "Child process %jd stopped. Continuing.\n", (intmax_t) fgPid);
          }
        }

      }
    }
    
_endPrompt:

    free(inputRedirectFilename);
    free(outputRedirectFilename);

    for (size_t i = 0; i < wordListLen; i++) {
      if (wordList[i] == NULL) break;
      free(wordList[i]);
    }

  }

  return 0;
}

char *expandWord(char *word) {
  // Home Expansion
  char home[] = "~/";
  if (strncmp(word, home, strlen(home)) == 0) {
    char *home = getEnvOrDefaultVal("HOME", ""),
         *expandedString = malloc(strlen(home) + strlen(word) + 1);
    strcpy(expandedString, home);
    char *remainingString = calloc(strlen(word), sizeof word[0]);
    strncpy(remainingString, word+1, strlen(word)-1);
    strncat(expandedString, remainingString, strlen(remainingString));
    word = realloc(word, strlen(expandedString) + 1);
    strcpy(word, expandedString);
    free(remainingString);
    free(expandedString);
  }
  
  // Smallsh PID Expansion
  intmax_t pid = getpid();
  char *pidStr = malloc(sizeof pid + 1);    
  sprintf(pidStr, "%jd", pid);
  strSub(&word, "$$", pidStr);
  free(pidStr);

  // Child PID Expansion
  if (lastBgPid > 0) {
    intmax_t bgPid = (intmax_t) lastBgPid;
    char *bgPidStr = malloc(sizeof bgPid + 1);
    sprintf(bgPidStr, "%jd", bgPid);
    strSub(&word, "$!", bgPidStr);
    free(bgPidStr);
  } else {
    strSub(&word, "$!", "");
  }

  // Exit & Signaled Status Expansion
  if (WIFEXITED(fgStatus)) {
    int exitStatus = WEXITSTATUS(fgStatus);
    char *exitStatusStr = malloc(sizeof exitStatus + 1);
    sprintf(exitStatusStr, "%d", exitStatus);
    strSub(&word, "$?", exitStatusStr);
    free(exitStatusStr);
  } else if (WIFSIGNALED(fgStatus)) {
    int signaledStatus = WTERMSIG(128 + fgStatus);
    char *signaledStatusStr = malloc(sizeof signaledStatus + 1);
    sprintf(signaledStatusStr, "%d", signaledStatus);
    strSub(&word, "$?", signaledStatusStr);
    free(signaledStatusStr);
  } else {
    strSub(&word, "$?", "0");
  }

  return word;
}

char *getEnvOrDefaultVal(char *env, char *defaultVal) {
  char *val = NULL;
  val = getenv(env);
  return val ? val : defaultVal;
}

char *strSub(char *restrict *restrict haystack, char const *restrict needle, char const *restrict sub) {
  // From video provided by professor - https://www.youtube.com/watch?v=-3ty5W_6-IQ
  char *str = *haystack;
  size_t haystackLen = strlen(str);
  size_t const needleLen = strlen(needle),
               subLen = strlen(sub);

  for (; (str = strstr(str, needle));) {
    ptrdiff_t off = str - *haystack;
    // If size is growing, reallocate accordingly
    if (subLen > needleLen) {
      str = realloc(*haystack, sizeof *str * (haystackLen * subLen - needleLen + 1));
      if (!str) goto exit;
      *haystack = str;
      str = *haystack + off;
    }

    // Move chars following needle to new location
    memmove(str + subLen, str + needleLen, haystackLen + 1 - off - needleLen);
    // Replace occurrence of needle with sub
    memcpy(str, sub, subLen);

    haystackLen = haystackLen + subLen - needleLen;
    str += subLen;  // Increment str to avoid recursive substitutions if sub contains needle
  }

  str = *haystack;

  // If size is shrinking, reallocate accordingly
  if (subLen < needleLen) {
    str = realloc(*haystack, sizeof *str * (haystackLen + 1));
    if (!str) goto exit;
    *haystack = str;
  }

exit:
  return str;
}

void exitProgram(char *statusChar) {
  bool validStatus = true;
  for (size_t i = 0; i < strlen(statusChar); i++) if (!isdigit(statusChar[i])) validStatus = false;

  if (validStatus) {
    fprintf(stderr, "\nexit\n");
    if (kill(0, SIGINT) == -1) err(errno, "Failed to kill child processes.");
    char *ptr = NULL;
    int status = strtol(statusChar, &ptr, 10);
    exit(status);
  } else {
    err(-1, "Invalid argument provided to built-in exit command.");
  }
}

