#include "common.h"
#include "ipc.h"
#include "pa1.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <stdarg.h>


typedef struct {
    int read_fd;
    int write_fd;
} Pipe;

typedef struct {
    local_id id;
    int process_count;
    Pipe **pipes;
    FILE *events_log;
    FILE *pipes_log;
} IPC;


int get_physical_time(void) {
    return 0;
}

void log_event(FILE *events_log, const char *format, ...) {
    va_list args;
    va_start(args, format);
    
    if (events_log) {
        vfprintf(events_log, format, args);
        fprintf(events_log, "\n");
        fflush(events_log);
    }
    
    va_end(args);
}

void log_pipes_info(IPC *ipc_context) {
    if (!ipc_context || !ipc_context->pipes_log) {
        return;
    }
    
    fprintf(ipc_context->pipes_log, "=== Pipe descriptors for Process %d ===\n", ipc_context->id);
    
    for (int i = 0; i < ipc_context->process_count; i++) {
        for (int j = 0; j < ipc_context->process_count; j++) {
            if (i != j) { // Каналы только между разными процессами
                Pipe *pipe = &ipc_context->pipes[i][j];
                fprintf(ipc_context->pipes_log, 
                       "Pipe[%d][%d]: read_fd=%d, write_fd=%d %s\n",
                       i, j, 
                       pipe->read_fd, 
                       pipe->write_fd,
                       (pipe->read_fd == -1 || pipe->write_fd == -1) ? "(CLOSED)" : "(OPEN)");
            }
        }
    }
    fprintf(ipc_context->pipes_log, "========================================\n\n");
    fflush(ipc_context->pipes_log);
}

void create_all_pipes(int process_count, int pipes[][MAX_PROCESS_ID + 1][2]) {
    for (int i = 0; i < process_count; i++) {
        for (int j = 0; j < process_count; j++) {
            if (i != j) {
                if (pipe(pipes[i][j]) == -1) {
                    perror("pipe creation failed");
                    exit(1);
                }
                
                // Устанавливаем блокирующий режим
                int flags;
                flags = fcntl(pipes[i][j][0], F_GETFL, 0);
                fcntl(pipes[i][j][0], F_SETFL, flags & ~O_NONBLOCK);
                flags = fcntl(pipes[i][j][1], F_GETFL, 0);
                fcntl(pipes[i][j][1], F_SETFL, flags & ~O_NONBLOCK);
            }
        }
    }
}


// Функция для инициализации IPC с уже созданными пайпами
IPC* init_ipc_with_pipes(local_id id, int process_count, int pipes[][MAX_PROCESS_ID + 1][2]) {
    IPC *ipc_context = malloc(sizeof(IPC));
    if (!ipc_context) {
        perror("malloc IPC failed");
        exit(1);
    }
    
    ipc_context->id = id;
    ipc_context->process_count = process_count;
    
    ipc_context->pipes = malloc(process_count * sizeof(Pipe*));
    if (!ipc_context->pipes) {
        perror("malloc pipes array failed");
        exit(1);
    }
    
    for (int i = 0; i < process_count; i++) {
        ipc_context->pipes[i] = malloc(process_count * sizeof(Pipe));
        if (!ipc_context->pipes[i]) {
            perror("malloc pipes row failed");
            exit(1);
        }
        for (int j = 0; j < process_count; j++) {
            if (i != j) {
                // Используем переданные пайпы
                ipc_context->pipes[i][j].read_fd = pipes[i][j][0];
                ipc_context->pipes[i][j].write_fd = pipes[i][j][1];
            } else {
                ipc_context->pipes[i][j].read_fd = -1;
                ipc_context->pipes[i][j].write_fd = -1;
            }
        }
    }
    
    ipc_context->events_log = fopen("events.log", id == 0 ? "w" : "a");
    if (!ipc_context->events_log) {
        perror("fopen events.log failed");
        exit(1);
    }
    
    ipc_context->pipes_log = fopen("pipes.log", id == 0 ? "w" : "a");
    if (!ipc_context->pipes_log) {
        perror("fopen pipes.log failed");
        exit(1);
    }
    
    return ipc_context;
}


void close_unused_pipes(IPC *ipc_context) {
    if (!ipc_context) return;
    
    for (int i = 0; i < ipc_context->process_count; i++) {
        for (int j = 0; j < ipc_context->process_count; j++) {
            if (i != j) {
                // Закрываем каналы записи, которые не принадлежат текущему процессу
                if (i != ipc_context->id && ipc_context->pipes[i][j].write_fd != -1) {
                    close(ipc_context->pipes[i][j].write_fd);
                    ipc_context->pipes[i][j].write_fd = -1;
                }
                
                // Закрываем каналы чтения, которые не предназначены текущему процессу
                if (j != ipc_context->id && ipc_context->pipes[i][j].read_fd != -1) {
                    close(ipc_context->pipes[i][j].read_fd);
                    ipc_context->pipes[i][j].read_fd = -1;
                }
            }
        }
    }
}

void cleanup_ipc(IPC *ipc_context) {
    if (ipc_context) {
        for (int i = 0; i < ipc_context->process_count; i++) {
            for (int j = 0; j < ipc_context->process_count; j++) {
                if (ipc_context->pipes[i][j].read_fd != -1) {
                    close(ipc_context->pipes[i][j].read_fd);
                }
                if (ipc_context->pipes[i][j].write_fd != -1) {
                    close(ipc_context->pipes[i][j].write_fd);
                }
            }
        }
        
        for (int i = 0; i < ipc_context->process_count; i++) {
            free(ipc_context->pipes[i]);
        }
        free(ipc_context->pipes);
        
        if (ipc_context->events_log) fclose(ipc_context->events_log);
        if (ipc_context->pipes_log) fclose(ipc_context->pipes_log);
        
        free(ipc_context);
    }
}



int send(void *self, local_id dst, const Message *msg) {
    IPC *ipc = (IPC *)self;
    
    if (dst < 0 || dst >= ipc->process_count || dst == ipc->id) {
        return -1;
    }
    
    int write_fd = ipc->pipes[ipc->id][dst].write_fd;
    if (write_fd < 0) {
        return -1;
    }
    
    size_t total_len = sizeof(MessageHeader) + msg->s_header.s_payload_len;
    ssize_t bytes_written = write(write_fd, msg, total_len);
    
    if (bytes_written != (ssize_t)total_len) {
        return -1;
    }
    
    return 0;
}

int send_multicast(void *self, const Message *msg) {
    IPC *ipc = (IPC *)self;
    
    for (local_id i = 0; i < ipc->process_count; i++) {
        if (i != ipc->id) {
            if (send(self, i, msg) != 0) {
                return -1;
            }
        }
    }
    
    return 0;
}

int receive(void *self, local_id from, Message *msg) {
    IPC *ipc = (IPC *)self;
    
    if (from < 0 || from >= ipc->process_count || from == ipc->id) {
        return -1;
    }
    
    int read_fd = ipc->pipes[from][ipc->id].read_fd;
    if (read_fd < 0) {
        return -1;
    }
    
    ssize_t bytes_read = read(read_fd, &msg->s_header, sizeof(MessageHeader));
    if (bytes_read != sizeof(MessageHeader)) {
        return -1;
    }
    
    if (msg->s_header.s_magic != MESSAGE_MAGIC) {
        return -1;
    }
    
    if (msg->s_header.s_payload_len > 0) {
        bytes_read = read(read_fd, msg->s_payload, msg->s_header.s_payload_len);
        if (bytes_read != msg->s_header.s_payload_len) {
            return -1;
        }
    }
    
    return 0;
}

int receive_any(void *self, Message *msg) {
    IPC *ipc = (IPC *)self;
    
    for (local_id i = 0; i < ipc->process_count; i++) { 
        if (i != ipc->id) {
            log_event(ipc->events_log, read_log, ipc->id ,i);
            if (receive(self, i, msg) == 0) {
                return 0;
            }
        }
    }
    
    return -1;
}


void child_process(local_id id, int process_count, int pipes[][MAX_PROCESS_ID + 1][2]) {
    // Создаем IPC для дочернего процесса с уже созданными пайпами
    IPC *ipc = init_ipc_with_pipes(id, process_count, pipes);
    if (!ipc) {
        exit(EXIT_FAILURE);
    }
    
    close_unused_pipes(ipc);
    
    // Фаза 1: Синхронизация запуска
    char started_msg[100];
    snprintf(started_msg, sizeof(started_msg), log_started_fmt, id, getpid(), getppid());
    
    log_event(ipc->events_log, log_started_fmt, id, getpid(), getppid());
    
    Message msg;
    msg.s_header.s_magic = MESSAGE_MAGIC;
    msg.s_header.s_type = STARTED;
    msg.s_header.s_payload_len = strlen(started_msg);
    memcpy(msg.s_payload, started_msg, msg.s_header.s_payload_len);
    
    if (send_multicast(ipc, &msg) != 0) {
        cleanup_ipc(ipc);
        exit(EXIT_FAILURE);
    }
    
    // Ждем STARTED от всех других процессов
    int received_started = 0;
    while (received_started < process_count - 1) {
        if (receive_any(ipc, &msg) == 0 && msg.s_header.s_type == STARTED) {
            received_started++;
        }
    }
    
    log_event(ipc->events_log, log_received_all_started_fmt, id);
    
    // Фаза 2: "Полезная" работа (в этой работе отсутствует)
    log_event(ipc->events_log, log_done_fmt, id);
    
    // Фаза 3: Синхронизация завершения
    char done_msg[100];
    snprintf(done_msg, sizeof(done_msg), log_done_fmt, id);
    
    msg.s_header.s_magic = MESSAGE_MAGIC;
    msg.s_header.s_type = DONE;
    msg.s_header.s_payload_len = strlen(done_msg);
    memcpy(msg.s_payload, done_msg, msg.s_header.s_payload_len);
    
    if (send_multicast(ipc, &msg) != 0) {
        cleanup_ipc(ipc);
        exit(EXIT_FAILURE);
    }
    
    // Ждем DONE от всех других процессов
    int received_done = 0;
    while (received_done < process_count - 1) {
        if (receive_any(ipc, &msg) == 0 && msg.s_header.s_type == DONE) {
            received_done++;
        }
    }
    
    log_event(ipc->events_log, log_received_all_done_fmt, id);
    
    cleanup_ipc(ipc);
    exit(EXIT_SUCCESS);
}


int main(int argc, char *argv[]) {
    int child_count = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            child_count = atoi(argv[i + 1]);
            if (child_count <= 0 || child_count > MAX_PROCESS_ID) {
                fprintf(stderr, "Invalid process count: %d\n", child_count);
                return EXIT_FAILURE;
            }
            break;
        }
    }
    
    if (child_count == 0) {
        fprintf(stderr, "Usage: %s -p X\n", argv[0]);
        return EXIT_FAILURE;
    }
    
    int total_processes = child_count + 1;
    
    // Создаем массив для хранения пайпов
    int pipes[MAX_PROCESS_ID + 1][MAX_PROCESS_ID + 1][2];
    
    // Создаем все пайпы ОДИН РАЗ в родительском процессе
    create_all_pipes(total_processes, pipes);
    
    // Создаем IPC для родительского процесса
    IPC *ipc = init_ipc_with_pipes(PARENT_ID, total_processes, pipes);
    if (!ipc) {
        fprintf(stderr, "Failed to create IPC\n");
        return EXIT_FAILURE;
    }
    close_unused_pipes(ipc);

    
    // Логируем информацию о пайпах
    log_pipes_info(ipc);


    // Создаем дочерние процессы
    for (local_id i = 1; i < total_processes; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // Дочерний процесс - наследует все открытые файловые дескрипторы
            child_process(i, total_processes, pipes);
        } else if (pid < 0) {
            perror("fork");
            cleanup_ipc(ipc);
            return EXIT_FAILURE;
        }
    }
    
    // Родитель ждет STARTED и DONE от всех дочерних процессов
    Message msg;
    int received_started = 0;
    int received_done = 0;
    
    while (received_started < child_count || received_done < child_count) {
        if (receive_any(ipc, &msg) == 0) {
            if (msg.s_header.s_type == STARTED) {
                received_started++;
            } else if (msg.s_header.s_type == DONE) {
                received_done++;
            }
        }
    }
    
    // Ждем завершения всех дочерних процессов
    for (int i = 0; i < child_count; i++) {
        int status;
        wait(&status);
    }
    
    cleanup_ipc(ipc);
    return EXIT_SUCCESS;
}
