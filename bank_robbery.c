/**
 * @file     bank_robbery.c
 * @Author   Michael Kosyakov and Evgeniy Ivanov (ifmo.distributedclass@gmail.com)
 * @date     March, 2014
 * @brief    Toy implementation of bank_robbery(), don't do it in real life ;)
 *
 * Students must not modify this file!
 */

 #include "banking.h"
 #include "ipc.h"
 #include "pa2345.h"
 #include <unistd.h>
 #include <stdio.h>
 #include <string.h>
 #include <stdio.h>

 // Структура для хранения состояния процесса
 typedef struct {
     local_id id;
     int pipe_fd[MAX_PROCESS_ID + 1][2]; // [from][to]
     balance_t balance;
     BalanceHistory balance_history;
     int max_id;
 } ProcessData;
 
 void transfer(void *parent_data, local_id src, local_id dst, balance_t amount) {
     ProcessData *data = (ProcessData *)parent_data;
     
     // Создаем TransferOrder
     TransferOrder order;
     order.s_src = src;
     order.s_dst = dst;
     order.s_amount = amount;
     
     // Создаем сообщение
     Message msg;
     msg.s_header.s_magic = MESSAGE_MAGIC;
     msg.s_header.s_type = TRANSFER;
     msg.s_header.s_payload_len = sizeof(TransferOrder);
     msg.s_header.s_local_time = get_physical_time();
     memcpy(msg.s_payload, &order, sizeof(TransferOrder));
     
     // Отправляем сообщение процессу-источнику
     send(data, src, &msg);
     
     // Ждем ACK от процесса-получателя
     Message ack_msg;
     while (1) {
         if (receive(data, dst, &ack_msg) == 0) {
             if (ack_msg.s_header.s_type == ACK) {
                 break;
             }
         }
     }
 }
 
 // Функция для обработки сообщений в дочерних процессах
 void child_process(ProcessData *data, balance_t initial_balance) {
     data->balance = initial_balance;
     
     // Инициализируем историю баланса
     data->balance_history.s_id = data->id;
     data->balance_history.s_history_len = 0;
     
     // Записываем начальное состояние
     BalanceState initial_state;
     initial_state.s_balance = initial_balance;
     initial_state.s_time = get_physical_time();
     initial_state.s_balance_pending_in = 0;
     
     data->balance_history.s_history[0] = initial_state;
     data->balance_history.s_history_len = 1;
     
     // Логируем старт
     printf(log_started_fmt, 
            get_physical_time(), data->id, getpid(), getppid(), initial_balance);
     
     // Отправляем STARTED родителю
     Message started_msg;
     started_msg.s_header.s_magic = MESSAGE_MAGIC;
     started_msg.s_header.s_type = STARTED;
     started_msg.s_header.s_payload_len = 0;
     started_msg.s_header.s_local_time = get_physical_time();
     
     send_multicast(data, &started_msg);
     
     // Основной цикл обработки сообщений
     int done_received = 0;
     while (!done_received) {
         Message msg;
         if (receive_any(data, &msg) == 0) {
             switch (msg.s_header.s_type) {
                 case TRANSFER: {
                     TransferOrder *order = (TransferOrder *)msg.s_payload;
                     
                     if (data->id == order->s_src) {
                         // Мы - источник перевода
                         if (data->balance >= order->s_amount) {
                             data->balance -= order->s_amount;
                             
                             // Логируем отправку
                             printf(log_transfer_out_fmt,
                                    get_physical_time(), data->id, order->s_amount, order->s_dst);
                             
                             // Пересылаем сообщение получателю
                             send(data, order->s_dst, &msg);
                         }
                     } else if (data->id == order->s_dst) {
                         // Мы - получатель перевода
                         data->balance += order->s_amount;
                         
                         // Логируем получение
                         printf(log_transfer_in_fmt,
                                get_physical_time(), data->id, order->s_amount, order->s_src);
                         
                         // Отправляем ACK родителю
                         Message ack_msg;
                         ack_msg.s_header.s_magic = MESSAGE_MAGIC;
                         ack_msg.s_header.s_type = ACK;
                         ack_msg.s_header.s_payload_len = 0;
                         ack_msg.s_header.s_local_time = get_physical_time();
                         
                         send(data, PARENT_ID, &ack_msg);
                     }
                     
                     // Обновляем историю баланса
                     BalanceState new_state;
                     new_state.s_balance = data->balance;
                     new_state.s_time = get_physical_time();
                     new_state.s_balance_pending_in = 0;
                     
                     data->balance_history.s_history[data->balance_history.s_history_len] = new_state;
                     data->balance_history.s_history_len++;
                     
                     break;
                 }
                 
                 case STOP: {
                     // Отправляем DONE родителю
                     Message done_msg;
                     done_msg.s_header.s_magic = MESSAGE_MAGIC;
                     done_msg.s_header.s_type = DONE;
                     done_msg.s_header.s_payload_len = 0;
                     done_msg.s_header.s_local_time = get_physical_time();
                     
                     send(data, PARENT_ID, &done_msg);
                     
                     // Ждем DONE от всех процессов
                     int done_count = 0;
                     while (done_count < data->max_id - 1) {
                         Message temp_msg;
                         if (receive_any(data, &temp_msg) == 0) {
                             if (temp_msg.s_header.s_type == DONE) {
                                 done_count++;
                             }
                         }
                     }
                     
                     // Отправляем историю баланса родителю
                     Message history_msg;
                     history_msg.s_header.s_magic = MESSAGE_MAGIC;
                     history_msg.s_header.s_type = BALANCE_HISTORY;
                     history_msg.s_header.s_payload_len = sizeof(BalanceHistory);
                     history_msg.s_header.s_local_time = get_physical_time();
                     
                     memcpy(history_msg.s_payload, &data->balance_history, sizeof(BalanceHistory));
                     send(data, PARENT_ID, &history_msg);
                     
                     done_received = 1;
                     break;
                 }
             }
         }
     }
     
     // Логируем завершение
     printf(log_done_fmt, get_physical_time(), data->id, data->balance);
 }
 
 int main(int argc, char *argv[]) {
     // Парсинг аргументов
     if (argc < 4) {
         fprintf(stderr, "Usage: %s -p N balance1 balance2 ... balanceN\n", argv[0]);
         return 1;
     }
     
     int num_children = atoi(argv[2]);
     if (argc != 3 + num_children) {
         fprintf(stderr, "Invalid number of balance arguments\n");
         return 1;
     }
     
     // Инициализация структур данных
     ProcessData parent_data;
     parent_data.id = PARENT_ID;
     parent_data.max_id = num_children;
     
     // Создание pipe'ов и дочерних процессов
     // ... (код из первой лабораторной работы)
     
     // Родительский процесс
     if (getpid() == /* parent pid */) {
         // Ждем STARTED от всех дочерних процессов
         int started_count = 0;
         Message msg;
         while (started_count < num_children) {
             if (receive_any(&parent_data, &msg) == 0) {
                 if (msg.s_header.s_type == STARTED) {
                     started_count++;
                 }
             }
         }
         
         // Выполняем переводы
         bank_robbery(&parent_data, num_children);
         
         // Отправляем STOP всем дочерним процессам
         Message stop_msg;
         stop_msg.s_header.s_magic = MESSAGE_MAGIC;
         stop_msg.s_header.s_type = STOP;
         stop_msg.s_header.s_payload_len = 0;
         stop_msg.s_header.s_local_time = get_physical_time();
         
         send_multicast(&parent_data, &stop_msg);
         
         // Собираем истории балансов
         AllHistory all_history;
         all_history.s_history_len = num_children;
         
         int history_count = 0;
         while (history_count < num_children) {
             Message history_msg;
             if (receive_any(&parent_data, &history_msg) == 0) {
                 if (history_msg.s_header.s_type == BALANCE_HISTORY) {
                     BalanceHistory *bh = (BalanceHistory *)history_msg.s_payload;
                     all_history.s_history[history_count] = *bh;
                     history_count++;
                 }
             }
         }
         
         // Выводим историю
         print_history(&all_history);
         
         // Ждем завершения дочерних процессов
         // ... (код из первой лабораторной работы)
     }
     // Дочерние процессы
     else {
         local_id child_id = /* определить ID процесса */;
         balance_t initial_balance = atoi(argv[3 + child_id - 1]);
         child_process(&parent_data, initial_balance);
     }
     
     return 0;
 }