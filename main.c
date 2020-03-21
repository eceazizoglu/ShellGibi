#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>            //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>

const char *sysname = "shellgibi";

// list of commands that we implemented
char command_names [8][256] = {"wiki", "alarm", "volume", "myjobs", "pause", "mybg", "myfg", "psvis"};

enum return_codes {
    SUCCESS = 0,
    EXIT = 1,
    UNKNOWN = 2,
};
struct command_t {
    char *name;
    bool background;
    bool auto_complete;
    int arg_count;
    char **args;
    char *redirects[3]; // in/out redirection
    struct command_t *next; // for piping
};

/**
 * Prints a command struct
 * @param struct command_t *
 */
int process_command(struct command_t *command);
int redirection_command(struct command_t *command);
int execute(struct command_t *command);
int execute_pipeline(struct command_t *command);
int alarm_clock(struct command_t *command);
int open_wikipedia(struct command_t *command);
int handle_volume(struct command_t *command);
int myjobs(struct command_t *command);
int pause_process(struct command_t *command);

char suggestion_list[1024][256];
int possible_commands_count = 0;

// gets all the possible commands
void get_possible_list(char *head, char *path) {
    DIR *d;
    struct dirent *dir;
    d = opendir(path);
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            // gets written characters from command line and compares with the commands in the path
            if (strncmp(dir->d_name, head, strlen(head)) == 0) {
                // puts matched commands to the suggestion_list character array
                strcpy(suggestion_list[possible_commands_count++], dir->d_name);
            }
        }
        closedir(d);
    }
}

// gets all the possible commands implemented by us
void get_possible_special_list(char *head) {
    for (int i = 0; i < 8; ++i) {
        if (strncmp(command_names[i], head, strlen(head)) == 0) {
            strcpy(suggestion_list[possible_commands_count++], command_names[i]);
        }
    }
}

// gets all the possible file names within the current directory
void get_possible_file_list(char *head) {
    head[strlen(head) - 1] = '\0';
    DIR *d;
    struct dirent *dir;
    char cwd[250];
    getcwd(cwd, sizeof(cwd)); // gets current directory
    d = opendir(cwd);
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            if (strncmp(dir->d_name, head, strlen(head)) == 0) {
                strcpy(suggestion_list[possible_commands_count++], dir->d_name);
            }
        }
        closedir(d);
    }
}

// creates possible paths
void populate_suggestion_list(char *head) {
    head[strlen(head) - 1] = '\0';
    char path1[100] = "/bin/";
    char path2[100] = "/usr/bin/";
    char path3[100] = "/usr/local/bin/";
    char path4[100] = "/sbin/";
    get_possible_list(head, path1);
    get_possible_list(head, path2);
    get_possible_list(head, path3);
    get_possible_list(head, path4);
    get_possible_special_list(head);
}

void print_command(struct command_t *command) {
    int i = 0;
    printf("Command: <%s>\n", command->name);
    printf("\tIs Background: %s\n", command->background ? "yes" : "no");
    printf("\tNeeds Auto-complete: %s\n", command->auto_complete ? "yes" : "no");
    printf("\tRedirects:\n");
    for (i = 0; i < 3; i++)
        printf("\t\t%d: %s\n", i, command->redirects[i] ? command->redirects[i] : "N/A");
    printf("\tArguments (%d):\n", command->arg_count);
    for (i = 0; i < command->arg_count; ++i)
        printf("\t\tArg %d: %s\n", i, command->args[i]);
    if (command->next) {
        printf("\tPiped to:\n");
        print_command(command->next);
    }
}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command) {
    if (command == 0) {
        return 0;
    }
    if (command->arg_count) {
        for (int i = 0; i < command->arg_count; ++i)
            free(command->args[i]);
        free(command->args);
    }
    for (int i = 0; i < 3; ++i)
        if (command->redirects[i])
            free(command->redirects[i]);
    if (command->next) {
        free_command(command->next);
        command->next = NULL;
    }
    free(command->name);
    free(command);
    return 0;
}

char auto_complete_command[256];

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt() {
    char cwd[1024], hostname[1024];
    gethostname(hostname, sizeof(hostname));
    getcwd(cwd, sizeof(cwd));
    printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
    return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command) {
    const char *splitters = " \t"; // split at whitespace
    int index, len;
    len = strlen(buf);
    while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
    {
        buf++;
        len--;
    }
    while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
        buf[--len] = 0; // trim right whitespace

    if (len > 0 && buf[len - 1] == '?') // auto-complete
        command->auto_complete = true;
    if (len > 0 && buf[len - 1] == '&') // background
        command->background = true;

    char *pch = strtok(buf, splitters);
    command->name = (char *) malloc(strlen(pch) + 1);
    if (pch == NULL)
        command->name[0] = 0;
    else
        strcpy(command->name, pch);

    command->args = (char **) malloc(sizeof(char *));

    int redirect_index;
    int arg_index = 0;
    char temp_buf[1024], *arg;
    while (1) {
        // tokenize input on splitters
        pch = strtok(NULL, splitters);
        if (!pch) break;
        arg = temp_buf;
        strcpy(arg, pch);
        len = strlen(arg);

        if (len == 0) continue; // empty arg, go for next
        while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
        {
            arg++;
            len--;
        }
        while (len > 0 && strchr(splitters, arg[len - 1]) != NULL) arg[--len] = 0; // trim right whitespace
        if (len == 0) continue; // empty arg, go for next

        // piping to another command
        if (strcmp(arg, "|") == 0) {
            struct command_t *c = malloc(sizeof(struct command_t));
            int l = strlen(pch);
            pch[l] = splitters[0]; // restore strtok termination
            index = 1;
            while (pch[index] == ' ' || pch[index] == '\t') index++; // skip whitespaces

            parse_command(pch + index, c);
            pch[l] = 0; // put back strtok termination
            command->next = c;
            continue;
        }

        // background process
        if (strcmp(arg, "&") == 0)
            continue; // handled before

        // handle input redirection
        redirect_index = -1;
        if (arg[0] == '<')
            redirect_index = 0;
        if (arg[0] == '>') {
            if (len > 1 && arg[1] == '>') {
                redirect_index = 2;
                arg++;
                len--;
            } else redirect_index = 1;
        }
        if (redirect_index != -1) {
            command->redirects[redirect_index] = malloc(len);
            strcpy(command->redirects[redirect_index], arg + 1);
            continue;
        }

        // normal arguments
        if (len > 2 && ((arg[0] == '"' && arg[len - 1] == '"')
                        || (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
        {
            arg[--len] = 0;
            arg++;
        }
        command->args = (char **) realloc(command->args, sizeof(char *) * (arg_index + 1));
        command->args[arg_index] = (char *) malloc(len + 1);
        strcpy(command->args[arg_index++], arg);
    }
    command->arg_count = arg_index;
    return 0;
}

void prompt_backspace() {
    putchar(8); // go back 1
    putchar(' '); // write empty over
    putchar(8); // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command) {
    int index = 0;
    char c;
    char buf[4096];
    static char oldbuf[4096];

    // tcgetattr gets the parameters of the current terminal
    // STDIN_FILENO will tell tcgetattr that it should write the settings
    // of stdin to oldt
    static struct termios backup_termios, new_termios;
    tcgetattr(STDIN_FILENO, &backup_termios);
    new_termios = backup_termios;
    // ICANON normally takes care that one line at a time will be processed
    // that means it will return if it sees a "\n" or an EOF or an EOL
    new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
    // Those new settings will be set to STDIN
    // TCSANOW tells tcsetattr to change attributes immediately.
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);


    //FIXME: backspace is applied before printing chars
    show_prompt();
    int multicode_state = 0;
    buf[0] = 0;
    while (1) {
        c = getchar();
        // printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

        if (c == 9) // handle tab
        {
            buf[index++] = '?'; // autocomplete
            break;
        }

        if (c == 127) // handle backspace
        {
            if (index > 0) {
                prompt_backspace();
                index--;
            }
            continue;
        }
        if (c == 27 && multicode_state == 0) // handle multi-code keys
        {
            multicode_state = 1;
            continue;
        }
        if (c == 91 && multicode_state == 1) {
            multicode_state = 2;
            continue;
        }
        if (c == 65 && multicode_state == 2) // up arrow
        {
            int i;
            while (index > 0) {
                prompt_backspace();
                index--;
            }
            for (i = 0; oldbuf[i]; ++i) {
                putchar(oldbuf[i]);
                buf[i] = oldbuf[i];
            }
            index = i;
            continue;
        } else
            multicode_state = 0;

        putchar(c); // echo the character
        buf[index++] = c;
        if (index >= sizeof(buf) - 1) break;
        if (c == '\n') // enter key
            break;
        if (c == 4) // Ctrl+D
            return EXIT;
    }
    if (index > 0 && buf[index - 1] == '\n') // trim newline from the end
        index--;
    buf[index++] = 0; // null terminate string


    strcpy(oldbuf, buf);

    parse_command(buf, command);

    // print_command(command); // DEBUG: uncomment for debugging

    // restore the old settings
    tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
    return SUCCESS;
}

int main() {
    while (1) {
        struct command_t *command = malloc(sizeof(struct command_t));
        memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

        int code;
        code = prompt(command);
        if (code == EXIT) break;

        code = process_command(command);
        if (code == EXIT) break;

        free_command(command);
    }

    printf("\n");
    return 0;
}

// recursive piping
int execute_pipeline(struct command_t *command) {
    int fd[2];
    pipe(fd);
    // if fork is not successful, execute command
    if (!fork()) {
        close(1); // close normal stdout
        dup(fd[1]); // make stdout same as fd[1]
        close(fd[0]);
        execute(command);
    } else { //
        close(0); // close normal stdin
        dup(fd[0]); // make stdin same as fd[0]
        close(fd[1]);
        struct command_t *next = command->next;
        next->args = (char **) realloc(
                next->args, sizeof(char *) * (next->arg_count += 2));
        // shift everything forward by 1
        for (int i = next->arg_count - 2; i > 0; --i)
            next->args[i] = next->args[i - 1];
        // set args[0] as a copy of name
        next->args[0] = strdup(next->name);
        // set args[arg_count-1] (last) to NULL
        next->args[next->arg_count - 1] = NULL;
        if (next->next) { // if next's next not null, do execute_pipeline
            execute_pipeline(next);
        } else { // if next's next is null, execute command
            execute(next);
        }
    }
    return 0;
}

// execute using execv()
int execute(struct command_t *command) {

    // paths where kernel commands are located
    char path1[100] = "/bin/";
    char path2[100] = "/usr/bin/";
    char path3[100] = "/usr/local/bin/";
    char path4[100] = "/sbin/";
    int res = 0;

    // gets current directory as a path
    if (command->name[0] == '.') {
        char cwd[250];
        getcwd(cwd, sizeof(cwd));
        strcat(cwd, command->name + 1);
        res = execv(cwd, command->args);
    }

    // concatenates command with possible path where it can occur ands executes using execv()
    strcat(path1, command->name);
    res = execv(path1, command->args);

    strcat(path2, command->name);
    res = execv(path2, command->args);

    strcat(path3, command->name);
    res = execv(path3, command->args);

    strcat(path4, command->name);
    res = execv(path4, command->args);

    if (res == -1) {
        printf("No such command found.\n");
    }
}

// redirection command for "<", ">" and ">>"
int redirection_command(struct command_t *command) {
    FILE *fp;
    FILE *com = NULL;
    char filename[100];
    char cmd[250] = "";
    int buffer_size = 128;
    char output[128];

    // concatenates args one by one to get the typed command till redirection symbol
    for (int i = 0; i < command->arg_count - 1; i++) {
        strcat(cmd, command->args[i]);
        strcat(cmd, " ");
    }
    // concatenates "<" to the command
    if (command->redirects[0] != NULL) {
        strcat(cmd, "<");
        strcat(cmd, command->redirects[0]);
        strcat(cmd, " ");
    }
    // concatenates ">" to the command
    if (command->redirects[1] != NULL) {
        strcat(cmd, ">");
        strcat(cmd, command->redirects[1]);
        strcat(cmd, " ");
    }
    // concatenates ">>" to the command
    if (command->redirects[2] != NULL) {
        strcat(cmd, ">>");
        strcat(cmd, command->redirects[2]);
        strcat(cmd, " ");
    }
    // initiate pipe streams and executes command
    fp = popen(cmd, "r");

    if (command->redirects[0] != NULL) {
        strcpy(filename, command->redirects[0]);
        // opens file in read mode
        com = fopen(filename, "r");
    }
    if (command->redirects[1] != NULL) {
        strcpy(filename, command->redirects[1]);
        // opens file in write mode
        com = fopen(filename, "w");
    }
    if (command->redirects[2] != NULL) {
        strcpy(filename, command->redirects[2]);
        // opens file in append mode
        com = fopen(filename, "a");
    }
    //  reads characters from a given file stream source into an array of characters
    while (fgets(output, buffer_size, fp) != NULL) {
        // writes character array to a file
        fprintf(com, "%s", output);
        printf("%s", output);
    }
    fclose(com);
    fclose(fp);
    pclose(fp);

    return 0;
}

int process_command(struct command_t *command) {
    if (command->auto_complete) {
        possible_commands_count = 0;
        if(command->next!=NULL) {
            command = command->next;
            command->auto_complete=true;
            process_command(command);
        } else if (command->arg_count == 0 &&
            (command->redirects[0] == NULL && command->redirects[1] == NULL && command->redirects[2] == NULL)) {
            populate_suggestion_list(command->name);
            printf("\n");
            if (possible_commands_count==1) {
                printf("%s\n", suggestion_list[0]);
            } else {
                for (int i = 0; i < possible_commands_count; ++i) {
                    printf("%s\t", suggestion_list[i]);
                    if (i % 5 == 4) {
                        printf("\n");
                    }
                }
                printf("\n");
            }
            strncpy(auto_complete_command, command->name, strlen(command->name));
        } else if (!(command->redirects[0] == NULL && command->redirects[1] == NULL && command->redirects[2] == NULL)) {
            char head[256];
            if(command->redirects[0] != NULL){
                strcpy(head,command->redirects[0]);
            } else if(command->redirects[1] != NULL){
                strcpy(head,command->redirects[1]);
            } else if(command->redirects[2] != NULL){
                strcpy(head,command->redirects[2]);
            }
            get_possible_file_list(head);
            printf("\n");
            if (possible_commands_count==1) {
                printf("%s\n", suggestion_list[0]);
            } else {
                for (int i = 0; i < possible_commands_count; ++i) {
                    printf("%s\t", suggestion_list[i]);
                    if (i % 5 == 4) {
                        printf("\n");
                    }
                }
                printf("\n");
            }
            strncpy(auto_complete_command, command->name, strlen(command->name));
        } else {
            get_possible_file_list(command->args[command->arg_count - 1]);
            printf("\n");
            if (possible_commands_count==1) {
                printf("%s\n", suggestion_list[0]);
            } else {
                for (int i = 0; i < possible_commands_count; ++i) {
                    printf("%s\t", suggestion_list[i]);
                    if (i % 5 == 4) {
                        printf("\n");
                    }
                }
                printf("\n");
            }
            strncpy(auto_complete_command, command->name, strlen(command->name));
        }
        return 0;
    }

    int r;
    if (strcmp(command->name, "") == 0) return SUCCESS;

    if (strcmp(command->name, "exit") == 0)
        return EXIT;

    if (strcmp(command->name, "cd") == 0) {
        if (command->arg_count > 0) {
            r = chdir(command->args[0]);
            if (r == -1)
                printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
            return SUCCESS;
        }
    }

    pid_t pid = fork();
    if (pid == 0) // child
    {
        /// This shows how to do exec with environ (but is not available on MacOs)
        // extern char** environ; // environment variables
        // execvpe(command->name, command->args, environ); // exec+args+path+environ

        /// This shows how to do exec with auto-path resolve
        // add a NULL argument to the end of args, and the name to the beginning
        // as required by exec

        // increase args size by 2
        command->args = (char **) realloc(
                command->args, sizeof(char *) * (command->arg_count += 2));

        // shift everything forward by 1
        for (int i = command->arg_count - 2; i > 0; --i)
            command->args[i] = command->args[i - 1];

        // set args[0] as a copy of name
        command->args[0] = strdup(command->name);
        // set args[arg_count-1] (last) to NULL
        command->args[command->arg_count - 1] = NULL;

        if (command->next != NULL) {
            execute_pipeline(command);
        }

        if (strcmp(command->name, "wiki")==0) // this is our first custom command. For more info
        {                                     // check the function open_Wikipedia.
            open_wikipedia(command);
            return SUCCESS;
        }

        if (strcmp(command->name, "volume")==0) // this is our second custom command. For more info
        {                                      // check the function handle_volume.
            handle_volume(command);
            return SUCCESS;
        }

        if (strcmp(command->name, "alarm")==0) // this is the alarm clock part. For more info
                                               // check the function alarm_clock.
        {
            alarm_clock(command);
            return SUCCESS;
        }

        if (strcmp(command->name, "myjobs")==0) // this is for listing the running jobs. For more info
                                                // check the function myjobs.
        {
            myjobs(command);
            return SUCCESS;
        }

        if (strcmp(command->name, "pause")==0)  // this is for pausing a process givent its pid. For more info
                                                // check the function pause_process.
        {
            pause_process(command);
            return SUCCESS;
        }

        if (!(command->redirects[0] == NULL && command->redirects[1] == NULL && command->redirects[2] == NULL)) {
            redirection_command(command);
        } else {
            execute(command);
        }
//        execvp(command->name, command->args); // exec+args+path

        exit(0);
        // TODO: do your own exec with path resolving using execv()
    } else {
        if (!command->background)
            wait(0); // wait for child process to finish
        return SUCCESS;
    }

    // TODO: your implementation here

    printf("-%s: %s: command not found\n", sysname, command->name);
    return UNKNOWN;
}


// alarm_clock gives the command to crontab in its appropriate format.

int alarm_clock(struct command_t *command) {
    char time[6]; // time is in hh.mm
    strcpy(time, command->args[1]);
    const char s[2] = ".";  // seperator
    char* token = strtok(time, s);
    int minute = -1, hour = -1, count = 0;

    while(token != NULL) {
        int time_unit = atoi(token);
        if (count ==0) {    // time_unit indicates hour
            hour = time_unit;
        }
        else if(count==1) {  // time_unit indicates hour
            minute = time_unit;
        }
        token = strtok(NULL,s);
        count++;
    }

    char input[200];
    char file[200] = "./";
    strcat(file, command->args[2]);

    char *real_path;
    real_path = realpath(file, NULL);

    sprintf(input, "%d %d %c %c %c %s %s", minute, hour, '*', '*', '*', "mpg321", real_path);

    FILE *file2;
    DIR *directory;
    struct dirent *dir;
    directory = opendir("/var/spool/cron/crontabs");

    while((dir = readdir(directory)) != NULL) {
        if (strcmp(dir->d_name, "..") == 0 || strcmp(dir->d_name, ".") == 0)
            continue;
        char location[200] = "/var/spool/cron/crontabs/";
        strcat(location, dir->d_name);
        file2 = fopen(location, "a");
        fprintf(file2, "%s\n", input);
        fclose(file2);
    }

    return 1;
}

// if the command is only wiki, it opens the home page of wikipedia,
// if it is something like "wiki didem" it searches for didem in wikipedia.

int open_wikipedia(struct command_t *command) {
    char link[200] = "https://www.wikipedia.org/wiki/";
    if(command->args[1] != NULL) {
        strcat(link, command->args[1]);
        execlp("xdg-open", " ", link, NULL); //homepage
    }
    else {
        execlp("xdg-open", "xdg-open", link,  NULL); //searching
    }

    return 1;
}

// for handling volume operations we used amixer command seen below.

int handle_volume(struct command_t *command) {
    if(strcmp(command->args[1], "up") == 0) {
        printf("volume is up\n");
        execlp("amixer", " ","-D", "pulse", "sset","Master", "5%+", "--quiet", NULL); // "volume up" command increases volume by 5%
    }else if(strcmp(command->args[1], "down") == 0) {
        printf("volume is down\n");
        execlp("amixer","amixer", "-D", "pulse", "sset","Master", "5%-", "--quiet", NULL); // "volume down" command decreases volume by 5%
    }
    else if(strcmp(command->args[1], "mute") == 0) {
        printf("muted\n");
        execlp("amixer","amixer", "-D", "pulse", "sset","Master", "0%", "--quiet",NULL); // "volume mute" command mutes the volume alltogether
    }
    else if(strcmp(command->args[1], "unmute") == 0) {
        printf("unmuted\n");
        execlp("amixer","amixer", "-D", "pulse", "sset","Master", "50%", "--quiet",NULL); // "volume unmute" gets the volume to 50%
    }
    return 1;
}

// outputs all the jobs for user

int myjobs(struct command_t *command) {

    char output[200];
    char hostname[1024];
    gethostname(hostname, sizeof(hostname)); // for getting the name of the user
    strcpy(output, getenv("USER"));
    execlp("ps", "ps", "-u", output,  NULL);

    return 1;
}


int pause_process(struct command_t *command) {

    execlp("kill", "kill", "-STOP", command->args[1],  NULL);

    return 1;
}
