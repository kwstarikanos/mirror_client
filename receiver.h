#ifndef RECEIVER_H
#define RECEIVER_H

extern char *common_dir, *input_dir, *mirror_dir, *log_file;
extern unsigned long int buffer_size;
extern int id, fd_log_file;

extern unsigned int digits(int n);

void receiver(int sid);

#endif
