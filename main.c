#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

#define MAX_CMD_SIZE 2048       // max assumed size of a command
#define MAX_ARG_SIZE 512        // max assumed size of the arguments
int globalStatus = 0;           // records the exit status for processes
int childExit = 0;              // global var for background child handling
int foreground = 0;             // bool for foreground only mode

// struct for managing the command
// includes command, arguments, file in, file out, running in background or foreground
struct Cmd {
    char *cmd;  
    char *arg;
    char *in;
    char *out;
    int bkgr;  // simple int that acts as a bool
};

// signal handler for background child processes
// modeled after an example provided in the signals explorations 
void handleSIGCHLD(int signo) {
    childExit = 1;
    return;
}

// signal handler for SIGINT for foreground children
void childHandleSIGINT(int signo) {
    childExit = 1;
    exit(0);
}

// signal handler for SIGTSTP aka foreground mode
// it catches the signal and it toggles between modes
void handleSIGTSTP(int signo) {
    if (foreground == 0) {
        foreground = 1;
        const char *message = "Entering foreground-only mode (& is now ignored)\n";         // uses write instead of printf
        write(STDOUT_FILENO, message, strlen(message));
        fflush(stdout);
    } else {
        foreground = 0;
        const char *message = "Exiting foreground-only mode\n";
        write(STDOUT_FILENO, message, strlen(message));
        fflush(stdout);
    }
    return;
}

// collects zombie child and reaps it
void reapChild() {
    int status;
    pid_t pid;

    // uses WNOHANG and checks if the process was killed via signal
    pid = waitpid(0, &status, WNOHANG);
    if (WIFEXITED(status) && (pid != 0 && pid != -1)) {
        printf("Background process %d finished with exit status: %d\n", pid, WEXITSTATUS(status));
        fflush(stdout);
    } else if (WIFSIGNALED(status) && (pid != 0 && pid != -1)) {
        printf("Background process %d was killed by signal %d\n", pid, WTERMSIG(status));       // returns pid and signal id
        fflush(stdout);
    }

    return;  
}

/*
 * this function handles parsing the user input and converting it into a custom struct that is easily passed
 * to program execution. It prints the ":  " for the terminal and also handles variable expansion for '$$'. In
 * the case of variable expansion, it replaces '$$' with the process ID of the small shell program. This code 
 * also handles tokenizing the input. This is fairly straightforward EXCEPT FOR HANDLING &. Please refer to the
 * bottom of the function for a detailed look into & parsing. 
 */
struct Cmd *getUserCommand() {
    printf(":  ");
    fflush(stdout);                                     // Ensures the prompt is printed immediately
    char input[MAX_CMD_SIZE];                           // Temporary array to store input
    char temp[MAX_ARG_SIZE];                            // Temporary buffer for variable expansion
    int found;

    // allocate and creates the command
    struct Cmd *comm = malloc(sizeof(struct Cmd));
    comm->cmd = comm->arg = comm->in = comm->out = NULL;
    comm->bkgr = 0;

    // get input
    fgets(input, MAX_CMD_SIZE, stdin);

    // if the iput is a comment or an empty line, return the empty comm
    if (input[0] == '#' || input[0] == '\n') {
        return comm;
    }

    // parse command (always first token)
    char *token = strtok(input, " \n");
    comm->cmd = calloc(strlen(token) + 1, sizeof(char));
    strcpy(comm->cmd, token);

    // parse arguments (not denoted with <, >, or &)
    token = strtok(NULL, " \n");

    // ensures that arg remains null unless there is a valid arg
    if (token != NULL && token[0] != '<' && token[0] != '>' && token[0] != '&')
        comm->arg = calloc(MAX_ARG_SIZE, sizeof(char));

    // variable expansion portion of the func
    while (token != NULL && token[0] != '<' && token[0] != '>' && token[0] != '&') {
        found = 0;

        /*
         * iterates through the token and checks for occurences of $$,
         * if it finds $$ it replaces it with null characters and replaces it with the pid of the process
         */
        for (int i = 0; i < strlen(token); i++) {
            if (token[i] == '$' && i > 0 && token[i-1] == '$') {
                found = 1;
                token[i] = '\0';            // sets the null chars
                token[i-1] = '\0';
                snprintf(temp, sizeof(temp), "%s%d", token, getpid());      // replaces && with the pid
                strcat(comm->arg, temp);        // appends to the args with the replaced pid
                break;
            }
        }

        // appends tokens to args if found
        if (found == 0) {
            strcat(comm->arg, token);
        }
        strcat(comm->arg, " ");                 // ensures that args are still seperated 
        token = strtok(NULL, " \n");
    }

    if (comm->arg != NULL)
        comm->arg[strlen(comm->arg) - 1] = '\0';   // fixes an issue where extra spaces or newline were added to the end of arg

    // parse input by checking for the in symbol
    if (token != NULL && token[0] == '<') {
        token = strtok(NULL, " \n");
        
        // if found, add to command
        if (token) {
            comm->in = calloc(strlen(token) + 1, sizeof(char));
            strcpy(comm->in, token);
        }
        token = strtok(NULL, " \n");
    }

    // parse output by checking for out symbol
    if (token != NULL && token[0] == '>') {
        token = strtok(NULL, " \n");
        
        // if found, add to command
        if (token) {
            comm->out = calloc(strlen(token) + 1, sizeof(char));
            strcpy(comm->out, token);
        }
        token = strtok(NULL, " \n");
    }

    /*
     * PREFACE: This sucked
     * 
     * set background bool if & exists ONLY IF '&' IS THE ENTIRE TOKEN AT THE END.
     * 
     * EXPLANATION: this function checks the for the existence of &. if it exists 
     * it checks if it is the only char in the token and if it is at the end. it does
     * this by allocating a nextToken and checking its value. 
     * 
     */

    char *nextToken = strtok(NULL, " \n");
    if (token != NULL && strcmp(token, "&") == 0 && nextToken == NULL) {
        comm->bkgr = 1;
    } else if (token != NULL || nextToken != NULL) {
        // these if statements handle the case where both tokens need to be
        // added to the args. this allows & to be included in args
        if (token != NULL) {
            strcat(comm->arg, " ");
            strcat(comm->arg, token);
        }
        if (nextToken != NULL) {
            strcat(comm->arg, " ");
            strcat(comm->arg, nextToken);
        }
        token = strtok(NULL, " \n");
        
        // repeat until the entire thing is stored in args
        while (token != NULL) {
            strcat(comm->arg, " ");
            strcat(comm->arg, token);
            token = strtok(NULL, " \n");
        }
    }

    return comm;
}

// convert the args into a 2d array with NULL at the end for easier use with exec
char **convertString(struct Cmd *comm, const char *input) {
    char **tokenArray = malloc(MAX_ARG_SIZE * sizeof(char *));
    
    // if the args are NULL, set the first index element to cmd for use with exec
    if (input == NULL) {
        tokenArray[0] = comm->cmd;
        return tokenArray;
    }
    
    // otherwise tokenize and transform the input string into a 2d array
    char *temp = strdup(input);
    char *token = strtok(temp, " ");
    
    // add the command to the arg array for proper execution
    tokenArray[0] = strdup(comm->cmd);
    int count = 1;      // this reflects that count includes the cmd at the front

    // for each token, it will add to the 2d array
    while(token != NULL) {
        tokenArray[count] = strdup(token);
        count++;
        token = strtok(NULL, " ");
    }

    // append a NULL into the end of the array
    tokenArray[count] = NULL;
    free(temp);     // free temp array
    return tokenArray;
}

/* 
 * call and use execvp() to execute a non built in command
 * use fork() exec() and waitpid()
 * 
 * fork off a child
 * use execvp() to execute command
 * use PATH variable to loom for non-built in commands
 *      allow shell scripts to be executed
 * if a command fails, print error set exit status to 1
 * child process must terminate after executing a command
 */
void execOther(struct Cmd *comm) {
    char **argArray;
    
    // convert the command and args to a 2d array to use with exec
    argArray = convertString(comm, comm->arg);
    int status;

    // fork
    pid_t pid = fork();

    // fork execution
    if (pid == 0) {
        // child

        // signal handling
        if (comm->bkgr == 1)
            signal(SIGINT, SIG_IGN);    // if background, ignore signal
        else
            signal(SIGINT, childHandleSIGINT);  // otherwise handle signal as a foreground child

        signal(SIGTSTP, SIG_IGN);   // ignore SIGTSTP

        // file redirection for file in
        if (comm->in != NULL) {
            // open fd and error check
            int input_fd = open(comm->in, O_RDONLY);
            if (input_fd == -1) {
                perror("Error with file in");
                exit(1);
            }

            // set standard file input
            dup2(input_fd, STDIN_FILENO);
            close(input_fd);
        } else if (comm->in == NULL && comm->bkgr == 1) {
            int bkgr_fd = open("/dev/null", O_RDONLY);
            if (bkgr_fd == -1) {
                perror("Error with default background file in");
                exit(1);
            }
            
            // set standard file input
            dup2(bkgr_fd, STDIN_FILENO);
            close(bkgr_fd);
        }

        // file output
        if (comm->out != NULL) {
            // open fd and error check
            int output_fd = open(comm->out, O_WRONLY | O_TRUNC | O_CREAT, 0644);
            if (output_fd == -1) {
                perror("Error with file out");
                exit(1);
            }

            // set standard file input
            dup2(output_fd, STDOUT_FILENO);
            close(output_fd);
        } else if (comm->out == NULL && comm->bkgr == 1) {
            int bkgr_fd = open("/dev/null", O_WRONLY | O_TRUNC | O_CREAT, 0644);
            if (bkgr_fd == -1) {
                perror("Error with default background file in");
                exit(1);
            }
            
            // set standard file input
            dup2(bkgr_fd, STDOUT_FILENO);
            close(bkgr_fd);
        }

        // command execution
        if (execvp(comm->cmd, argArray) == -1) {
            perror("Error executing command");
            exit(1);
        }
        exit(0);
    }

    // background parent execution
    else if ((pid != 0) && comm->bkgr == 1) {
        printf("Started background process %d\n", pid);
        fflush(stdout);
    }

    // foreground parent execution
    else {
        // waits for child and captures exit state
        waitpid(pid, &status, 0);

        // check if the child exited normally and set status
        if (WIFEXITED(status))
            globalStatus = WEXITSTATUS(status);
        else
            globalStatus = 1;
    
        // child SIGINT print
        if (WIFSIGNALED(status) && (pid != 0 && pid != -1)) {
            printf("Foreground process %d was killed by signal %d\n", pid, WTERMSIG(status));
            fflush(stdout);
        }
    }

    return;
}

/*
 * This function takes in a command as input and handles the execution of the three built in 
 * functions. In the case that the function needs to be executed by exec(), it is passed to a 
 * different function.
 * 
 * Handles (built-in):
 *      quit
 *      cd
 *      status
 */
void execBasic(struct Cmd *comm) {
    if (comm->cmd == NULL) {                        // empty line or comment
        return;
    } else if (strcmp(comm->cmd, "exit") == 0) {    // built in quit, this terminates all processes and exits the shell
        // needs to kill all processes and then exit
        exit(0);
    } else if (strcmp(comm->cmd, "cd") == 0) {      // change directory to the $HOME path or the argument path
        if (comm->arg == NULL) {
            // if no args->direct to $HOME
            if (chdir(getenv("HOME")) == -1) {
                perror("Error");
                return;
            }
        } else {
            if (chdir(comm->arg) == -1) {
                perror("Error");
                return;
            }
        }
        char temp[MAX_ARG_SIZE];
        printf("%s\n", getcwd(temp, sizeof(temp)));       // current print current working directory
        fflush(stdout);
        return;
    } else if (strcmp(comm->cmd, "status") == 0) {      // print global status
        printf("Exit value: %d\n", globalStatus);
        return;
    } else {
        execOther(comm);                            // execute other non-built in commands
        return;
    }
}

// this is a simple function to print the data within the command struct
// this was implemented as a debugging tool has been extremely useful
void printCommand(struct Cmd *comm) {
    printf("\ncmd: %s ", comm->cmd);
    printf("arg: |%s| ", comm->arg);
    printf("in: %s ", comm->in);
    printf("out: %s ", comm->out);
    printf("bkgr: %d\n\n", comm->bkgr);
    fflush(stdout);
    return;
}

int main() {
    // setup signal handlers for the required signals and for background children
    signal(SIGCHLD, handleSIGCHLD);
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, handleSIGTSTP);

    // main while loop for the children
    while (1) {
        // reap defunct children
        if (childExit == 1) {
            childExit = 0;
            reapChild();
        }
        // get command input
        struct Cmd *input = getUserCommand();
        //foreground flag handler
        if (foreground == 1) {
            input->bkgr = 0;
        }
        //printCommand(input);  // useful for debugging
        execBasic(input);
    }
    return 0;
}
