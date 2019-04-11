#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <errno.h>
#include <zconf.h>
#include <signal.h>
#include <dirent.h>
#include <stdbool.h>
#include <fcntl.h>
#include <math.h>
#include <wait.h>
#include "hash.h"
#include "sender.h"
#include "receiver.h"

#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))

#define TRIES 2

typedef void *pointer;

Hashtable clientsHT = NULL;
char *common_dir = NULL, *input_dir = NULL, *mirror_dir = NULL, *log_file = NULL;
unsigned long int buffer_size = 0;
int id = 0;
bool quit = false;
FILE *logfile = NULL;

typedef struct Client {
    int id;
    pid_t sender;
    pid_t receiver;
    int sender_tries;
    int receiver_tries;
} *Client;

/**
 * Calculate number of digits of specific int.*/
unsigned int _digits(int n) {
    if (n == 0) return 1;
    return (unsigned int) floor(log10(abs(n))) + 1;
}

int _rmdir(char *dir) {
    int status = EXIT_FAILURE;
    __pid_t d_pid = 0;
    d_pid = fork();
    if (d_pid == 0) {
        execlp("rm", "rm", "-r", "-f", dir, NULL);
        fprintf(stderr, "\n%s:%d-[%s] execlp error: '%s'\n", __FILE__, __LINE__, dir, strerror(errno));
    } else if (d_pid > 0) {
        waitpid(d_pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return WEXITSTATUS(status);
        }
    } else {
        fprintf(stderr, "\n%s:%d-fork error: '%s'\n", __FILE__, __LINE__, strerror(errno));
    }
    return status;
}

void wrongOptionValue(char *opt, char *val) {
    fprintf(stderr, "\nWrong value [%s] for option '%s'\n", val, opt);
    exit(EXIT_FAILURE);
}

/**
 * Read options from command line*/
void readOptions(
        int argc,
        char **argv,
        int *id,                            /*id*/
        char **common_dir,                  /*common_dir*/
        char **input_dir,                   /*input_dir*/
        char **mirror_dir,                  /*input_dir*/
        unsigned long int *buffer_size,     /*buffer_size*/
        char **log_file                     /*log_file*/
) {
    int i;
    char *opt, *optVal;
    for (i = 1; i < argc; ++i) {
        opt = argv[i];
        optVal = argv[i + 1];
        if (strcmp(opt, "-n") == 0) {
            if (optVal != NULL && optVal[0] != '-') {
                *id = (int) strtol(optVal, NULL, 10);
            } else {
                wrongOptionValue(opt, optVal);
            }
        } else if (strcmp(opt, "-c") == 0) {
            if (optVal != NULL && optVal[0] != '-') {
                *common_dir = optVal;
            } else {
                wrongOptionValue(opt, optVal);
            }
        } else if (strcmp(opt, "-i") == 0) {
            if (optVal != NULL && optVal[0] != '-') {
                *input_dir = optVal;
            } else {
                wrongOptionValue(opt, optVal);
            }
        } else if (strcmp(opt, "-m") == 0) {
            if (optVal != NULL && optVal[0] != '-') {
                *mirror_dir = optVal;
            } else {
                wrongOptionValue(opt, optVal);
            }
        } else if (strcmp(opt, "-b") == 0) {
            if (optVal != NULL && optVal[0] != '-') {
                *buffer_size = (unsigned long int) strtol(optVal, NULL, 10);
            } else {
                wrongOptionValue(opt, optVal);
            }
        } else if (strcmp(opt, "-l") == 0) {
            if (optVal != NULL && optVal[0] != '-') {
                *log_file = optVal;
            } else {
                wrongOptionValue(opt, optVal);
            }
        }
    }
}

/**
 * @Signal_handler
 * Interupt or quit action*/
void sig_int_quit_action(int signal) {
    fprintf(stdout, "C[%d:%d]: SIGNAL(%d) EXITING...\n", id, getpid(), signal);
    quit = true;
}

/**
 * @Signal_handler
 * SIGCHLD action*/
void sig_chld_action(int signal, siginfo_t *info, void *context) {
    int status = 0;
    pid_t child_pid = info->si_pid;
    //fprintf(stdout, "C[%d:%d] SIGNAL(%d) CHILD[%d] TERMINATED!\n", id, getpid(), signal, child_pid);
    pid_t cpid = waitpid(child_pid, &status, 0);
    if (WIFEXITED(child_pid)) {
/*        fprintf(stdout, "C[%d:%d] SIGNAL(%d) CHILD[%d] TERMINATED WITH STATUS %d\n", id, getpid(), signal, cpid,
                WEXITSTATUS(status));*/
    }
}

/**
 * @Signal_handler
 * Child finish*/
void sig_usr_1_action(int signal, siginfo_t *info, void *context) {
    int target_client = info->si_value.sival_int;
    pid_t child_pid = info->si_pid;
    Client client = NULL;

    //write(fd, &signal, sizeof(int));
    //write(fd, &info, sizeof(siginfo_t *));
    //write(fd, context, sizeof(void *));

    if ((client = HT_Get(clientsHT, &target_client)) != NULL) {
        if (client->sender == child_pid) {
            fprintf(stdout, "C[%d:%d] SIGNAL(%d) SENDER[%d:%d] COMPLETE HIS JOB!\n", id, getpid(), signal,
                    target_client, child_pid);
        } else if (client->receiver == child_pid) {
            fprintf(stdout, "C[%d:%d] SIGNAL(%d) RECEIVER[%d:%d] COMPLETE HIS JOB!\n", id, getpid(), signal,
                    target_client, child_pid);
        } else {
            fprintf(stderr, "C[%d:%d] SIGNAL(%d) I don't recognize you %d:%d, tinos eisai esy ??\n",
                    id, getpid(), signal, child_pid, target_client);
        }
    }
}

/**
 * @Signal_handler
 * Child alarm timeout*/
void sig_usr_2_action(int signal, siginfo_t *info, void *context) {
    int target_client = info->si_value.sival_int;
    pid_t child_pid = info->si_pid;
    Client client = NULL;

    if ((client = HT_Get(clientsHT, &target_client)) != NULL) {
        if (client->sender == child_pid) {
            fprintf(stderr, "C[%d:%d] SIGNAL(%d) SENDER[%d:%d] FAIL!, %d remaining attempts...\n",
                    id, getpid(), signal, target_client, child_pid, client->sender_tries);
            if (client->sender_tries-- > 0) {
                client->sender = fork();
                if (client->sender < 0) {
                    fprintf(stderr, "\n%s:%d-Sender fork error: '%s'\n", __FILE__, __LINE__, strerror(errno));
                    exit(EXIT_FAILURE);
                }
                if (client->sender == 0) {
                    sender(target_client);
                    exit(EXIT_SUCCESS);
                }
            }
        } else if (client->receiver == child_pid) {
            fprintf(stderr, "C[%d:%d] SIGNAL(%d) RECEIVER[%d:%d] FAIL!, %d remaining attempts...\n",
                    id, getpid(), signal, target_client, child_pid, client->receiver_tries);

            if (client->receiver_tries-- > 0) {
                client->receiver = fork();
                if (client->receiver < 0) {
                    fprintf(stderr, "\n%s:%d Receiver fork error: '%s'\n", __FILE__, __LINE__, strerror(errno));
                    exit(EXIT_FAILURE);
                }
                if (client->receiver == 0) {
                    receiver(target_client);
                    exit(EXIT_SUCCESS);
                }
            }
        } else {
            fprintf(stderr, "C[%d:%d] SIGNAL(%d) I don't recognize you %d:%d, tinos eisai esy ??\n",
                    id, getpid(), signal, child_pid, target_client);
        }
    }
}

/**
 * @Callback HT Create*/
Client clientCreate(const int *id) {
    Client c = NULL;
    c = (Client) malloc(sizeof(struct Client));
    if (c != NULL) {
        c->id = *id;
        c->sender = 0;
        c->receiver = 0;
        c->sender_tries = TRIES;
        c->receiver_tries = TRIES;
    }
    return c;
}

/**
 * @Callback HT Compare*/
int clientCompare(Client c1, Client c2) {
    return c1->id != c2->id;
}

/**
 * @Callback HT Hash*/
unsigned long int clientHash(const int *id, unsigned long int capacity) {
    return *id % capacity;
}

/**
 * @Callback HT Destroy*/
void clientDestroy(Client client) {
    assert(client != NULL);
    free(client);
}

/**
 * @inotify_create_event
 * */
void create(char *filename) {
    struct stat s = {0};
    char id_file[PATH_MAX + 1], fn[strlen(filename) + 1], *f_suffix = NULL;
    int client_id = 0;
    Client client = NULL;
    strcpy(fn, filename);

    client_id = (int) strtol(strtok(filename, "."), NULL, 10);

    if (client_id > 0 && client_id != id) {
        f_suffix = strtok(NULL, "\0");
        if (f_suffix != NULL && !strcmp(f_suffix, "id")) {

            /* Construct id file*/
            if (sprintf(id_file, "%s/%s", common_dir, fn) < 0) {
                fprintf(stderr, "\n%s:%d-sprintf error\n", __FILE__, __LINE__);
                exit(EXIT_FAILURE);
            }

            if (!stat(id_file, &s)) {
                if (!S_ISDIR(s.st_mode)) {

                    /* Insert detected client into HT in order to remember that it has been processed.*/
                    if (HT_Insert(clientsHT, &client_id, &client_id, (void **) &client)) {

                        /* Create sender.*/
                        client->sender = fork();
                        if (client->sender == 0) {
                            sender(client_id);
                            exit(EXIT_SUCCESS);
                        } else if (client->sender < 0) {
                            fprintf(stderr, "\n%s:%d-Sender fork error: '%s'\n", __FILE__, __LINE__, strerror(errno));
                            exit(EXIT_FAILURE);
                        }

                        /* Create receiver.*/
                        client->receiver = fork();
                        if (client->receiver == 0) {
                            receiver(client_id);
                            exit(EXIT_SUCCESS);
                        } else if (client->receiver < 0) {
                            fprintf(stderr, "\n%s:%d-Receiver fork error: '%s'\n", __FILE__, __LINE__,
                                    strerror(errno));
                            exit(EXIT_FAILURE);
                        }

                    } else {
                        fprintf(stderr, "\n%s:%d-HT Client [%d] already exists!\n", __FILE__, __LINE__, client->id);
                    }
                }
            } else {
                fprintf(stderr, "\n%s:%d-[%s] stat error: '%s'\n", __FILE__, __LINE__, id_file, strerror(errno));
                exit(EXIT_FAILURE);
            }
        }
    }
}

/**
 * @inotify_delete_event
 * */
void destroy(char *filename) {
    char path[PATH_MAX + 1], *suffix = NULL, *prefix = NULL, *folder = NULL;
    int client_id = 0, status = 0, retries = 2;;
    prefix = strtok(filename, ".");
    client_id = (int) strtol(prefix, NULL, 10);
    if (client_id > 0 && client_id != id) {
        if (HT_Remove(clientsHT, &client_id, &client_id, true)) {
            folder = malloc(sizeof(char) * strlen(prefix) + 1);
            strcpy(folder, prefix);
            suffix = strtok(NULL, "\0");
            if (suffix != NULL && !strcmp(suffix, "id")) {

                /* Construct path.*/
                if (sprintf(path, "%s/%s", mirror_dir, folder) < 0) {
                    fprintf(stderr, "\n%s:%d-sprintf error\n", __FILE__, __LINE__);
                } else {
                    while ((status = _rmdir(path)) && retries-- > 0);
                    if (status == EXIT_SUCCESS) {
                        printf("\n-Dir '%s' removed because client has left!\n", path);
                    }
                }
            }
            free(folder);
        } else {
            fprintf(stderr, "\n%s:%d-HT_Remove error\n", __FILE__, __LINE__);
        }
    }
}

int main(int argc, char *argv[]) {
    char event_buffer[EVENT_BUF_LEN], id_file[PATH_MAX + 1];
    static struct sigaction quit_action, child_error, child_finish, child_exit;
    int fd_i_notify = 0, ev, wd, status = 0, tries = 2;
    struct inotify_event *event = NULL;
    struct dirent *d = NULL;
    struct stat s = {0};
    FILE *file_id = NULL;
    ssize_t bytes = 0;
    DIR *dir = NULL;
    __pid_t wpid = 0;

    /* Read argument options from command line*/
    readOptions(argc, argv, &id, &common_dir, &input_dir, &mirror_dir, &buffer_size, &log_file);

    assert(id > 0);
    assert(common_dir != NULL);
    assert(input_dir != NULL);
    assert(mirror_dir != NULL);
    assert(buffer_size > 0);
    assert(log_file != NULL);

    printf("C[%d:%d] STARTUP\n", id, getpid());

    /* Check if input_dir directory exists.*/
    if (!stat(input_dir, &s)) {
        if (!S_ISDIR(s.st_mode)) {
            fprintf(stderr, "\n'%s' is not a directory!\n", input_dir);
            exit(EXIT_FAILURE);
        }
    } else {
        fprintf(stderr, "\n%s:%d-[%s] stat error: '%s'\n", __FILE__, __LINE__, input_dir, strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Check if mirror_dir directory already exists.*/
    if (!stat(mirror_dir, &s)) {
        fprintf(stderr, "\n'%s' directory already exists!\n", mirror_dir);
        exit(EXIT_FAILURE);
    } else {
        mkdir(mirror_dir, S_IRUSR | S_IWUSR | S_IXUSR);
    }

    /* Create common_dir*/
    mkdir(common_dir, S_IRUSR | S_IWUSR | S_IXUSR);

    if (sprintf(id_file, "%s/%d.id", common_dir, id) < 0) {
        fprintf(stderr, "\n%s:%d-sprintf error\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Check if [id].id file exists.*/
    if (!stat(id_file, &s)) {
        fprintf(stderr, "\n%s:%d-file '%s' already exists!\n", __FILE__, __LINE__, id_file);
        exit(EXIT_FAILURE);
    } else {
        file_id = fopen(id_file, "w");
        fprintf(file_id, "%d", (int) getpid());
        fclose(file_id);
    }

    /* Check if log_file file already exists.*/
    if (!stat(log_file, &s)) {
        fprintf(stderr, "\n%s:%d-file '%s' already exists!\n", __FILE__, __LINE__, log_file);
        exit(EXIT_FAILURE);
    } else {
        if ((logfile = fopen(log_file, "w")) == NULL) {
            fprintf(stderr, "\n%s:%d-[%s] open error: '%s'\n", __FILE__, __LINE__, log_file, strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    fprintf(logfile, "cl %d\n", id);
    fflush(logfile);

    /* Initialize inotify.*/
    if ((fd_i_notify = inotify_init()) < 0) {
        fprintf(stderr, "\n%s:%d-i-notify_init error: '%s'\n", __FILE__, __LINE__, strerror(errno));
    }

    /* Set custom signal handler for SIGINT (^c) & SIGQUIT (^\) signals.*/
    quit_action.sa_handler = sig_int_quit_action;
    sigfillset(&(quit_action.sa_mask));
    child_finish.sa_flags = SA_RESTART;
    sigaction(SIGINT, &quit_action, NULL);
    sigaction(SIGQUIT, &quit_action, NULL);
    sigaction(SIGHUP, &quit_action, NULL);


    /* Set custom signal handler for SIGCHLD signal.*/
    child_exit.sa_handler = (__sighandler_t) sig_chld_action;
    child_exit.sa_flags = SA_RESTART | SA_SIGINFO;
    sigfillset(&(child_exit.sa_mask));
    sigaction(SIGCHLD, &child_exit, NULL);


    /* Set custom signal handler for SIGUSR1 (Child finish normally) signal.*/
    child_finish.sa_handler = (__sighandler_t) sig_usr_1_action;
    child_finish.sa_flags = SA_RESTART | SA_SIGINFO;
    sigfillset(&(child_finish.sa_mask));
    sigaction(SIGUSR1, &child_finish, NULL);

    /* Set custom signal handler for SIGUSR2 (Child error or alarm timeout) signal.*/
    child_error.sa_handler = (__sighandler_t) sig_usr_2_action;
    child_error.sa_flags = SA_RESTART | SA_SIGINFO;
    sigfillset(&(child_error.sa_mask));
    sigaction(SIGUSR2, &child_error, NULL);

    /* Add common_dir at watch list to detect changes.*/
    wd = inotify_add_watch(fd_i_notify, common_dir, IN_CREATE | IN_DELETE);

    /* Initialize clients hashtable*/
    HT_Init(
            &clientsHT,
            100,
            512,
            (pointer (*)(pointer)) clientCreate,
            (int (*)(pointer, pointer)) clientCompare,
            (unsigned long (*)(pointer, unsigned long int)) clientHash,
            (unsigned long (*)(pointer)) clientDestroy
    );

    /* Search for not processed clients.*/
    if ((dir = opendir(common_dir))) {
        while ((d = readdir(dir))) {
            if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) {
                continue;
            }
            create(d->d_name);
        }
        closedir(dir);
    }

    /* Read i-notify events*/
    while (!quit) {
        if ((bytes = read(fd_i_notify, event_buffer, EVENT_BUF_LEN)) < 0) {
            fprintf(stderr, "\n%s:%d-i-notify event read error: '%s'\n", __FILE__, __LINE__, strerror(errno));
        }
        ev = 0;
        while (ev < bytes) {
            event = (struct inotify_event *) &event_buffer[ev];
            if (event->len) {
                if (event->mask & IN_CREATE) {
                    if (!(event->mask & IN_ISDIR)) {
                        create(event->name);
                    }
                } else if (event->mask & IN_DELETE) {
                    if (!(event->mask & IN_ISDIR)) {
                        destroy(event->name);
                    }
                }
                ev += EVENT_SIZE + event->len;
            }
        }
    }

    /* Remove common_dir from watch list.*/
    inotify_rm_watch(fd_i_notify, wd);

    /* Close the i-notify instance.*/
    close(fd_i_notify);

    /* Wait all childs*/
    while ((wpid = wait(&status)) > 0);

    fprintf(logfile, "dn\n");
    fflush(logfile);

    /* Delete id file.*/
    if (unlink(id_file) < 0) {
        fprintf(stderr, "\n%s:%d-[%s] unlink error: '%s'\n", __FILE__, __LINE__, id_file, strerror(errno));
    }

    while ((status = _rmdir(mirror_dir)) && tries-- > 0);

    if (status == EXIT_FAILURE) {
        fprintf(stderr, "\n%s:%d- _rmdir error'\n", __FILE__, __LINE__);
    }

    fclose(logfile);

    HT_Destroy(&clientsHT, true);
    return 0;
}
