#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mqueue.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

#define GLOBAL_MQ_NAME "/global_mq"
#define INDIVIDUAL_MQ_NAME_PREFIX "/mq_"
#define MAX_MSG_SIZE 1024
#define TASK_NAME_LEN 128
#define MAX_TASKS 10
#define NUM_CONFIGS 7
#define SCHEDULING_INTERVAL 10
#define BRUTE_FORCE_THRESHOLD 4

#define WEIGHT_THROUGHPUT 1.0
#define WEIGHT_CACHE      1.0
#define WEIGHT_MEMBW      1.0

typedef struct {
    int thread_count;
    double throughput;
    double flops;
    double cache_miss_rate;
    double mem_bw;
} profiling_data;

typedef struct task_kernel {
    uint64_t IPC[256];
    uint64_t cache_miss[256];
    uint64_t cache_reference[256];
    uint64_t throughput[256];
    uint64_t FLOPS[256];
    struct task_kernel *next;
} task_kernel;

typedef struct {
    char name[TASK_NAME_LEN];
    mqd_t mq;
    task_kernel *kernel_list;
    pthread_mutex_t lock;
    int active;
    profiling_data profiles[NUM_CONFIGS];
    int profiles_received;
    int profiling_complete;
    int optimal_thread_count;
    int scheduled_core;
    int scheduled;
} omp_task;

typedef struct {
    omp_task *task;
    char task_name[TASK_NAME_LEN];
} task_thread_arg;

omp_task global_tasks[MAX_TASKS];
int current_task_index = 0;

void brute_force_enum(omp_task **tasks, int n, int pos, int *current_comb, double *best_score, int *best_comb, double *indep_throughputs);

int parse_profiling_message(const char *msg, profiling_data *pdata) {
    const char *data_str = msg + 8;
    return sscanf(data_str, "%d,%lf,%lf,%lf,%lf",
                  &pdata->thread_count,
                  &pdata->throughput,
                  &pdata->flops,
                  &pdata->cache_miss_rate,
                  &pdata->mem_bw);
}

void save_profiling_data(omp_task *task) {
    char filename[TASK_NAME_LEN + 32];
    snprintf(filename, sizeof(filename), "task_%s_profile.dat", task->name);
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        perror("fopen save_profiling_data");
        return;
    }
    for (int i = 0; i < task->profiles_received; i++) {
        profiling_data *p = &task->profiles[i];
        fprintf(fp, "%d,%lf,%lf,%lf,%lf\n",
                p->thread_count,
                p->throughput,
                p->flops,
                p->cache_miss_rate,
                p->mem_bw);
    }
    fclose(fp);
    printf("Saved profiling data to %s\n", filename);
}

void load_profiling_data() {
    printf("load_profiling_data: Not implemented (placeholder)\n");
}

void insert_task_kernel(omp_task *task, task_kernel *new_kernel) {
    pthread_mutex_lock(&task->lock);
    if (task->kernel_list == NULL) {
        new_kernel->next = new_kernel;
        task->kernel_list = new_kernel;
    } else {
        task_kernel *current = task->kernel_list;
        while (current->next != task->kernel_list) {
            current = current->next;
        }
        current->next = new_kernel;
        new_kernel->next = task->kernel_list;
    }
    pthread_mutex_unlock(&task->lock);
}

void* task_handler(void *arg) {
    task_thread_arg *targ = (task_thread_arg*) arg;
    omp_task *task = targ->task;
    char *task_name = targ->task_name;
    strncpy(task->name, task_name, TASK_NAME_LEN - 1);
    task->name[TASK_NAME_LEN - 1] = '\0';
    char mq_name[TASK_NAME_LEN + 16];
    snprintf(mq_name, sizeof(mq_name), "%s%s", INDIVIDUAL_MQ_NAME_PREFIX, task_name);
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = MAX_MSG_SIZE;
    attr.mq_curmsgs = 0;
    mqd_t ind_mq = mq_open(mq_name, O_CREAT | O_RDWR, 0644, &attr);
    if (ind_mq == (mqd_t)-1) {
        perror("mq_open individual");
        free(targ);
        pthread_mutex_lock(&task->lock);
        task->active = 0;
        pthread_mutex_unlock(&task->lock);
        pthread_exit(NULL);
    }
    task->mq = ind_mq;
    printf("Task Handler: Created individual mqueue '%s' for task '%s'\n", mq_name, task_name);
    task_kernel *new_kernel = (task_kernel*) malloc(sizeof(task_kernel));
    if (!new_kernel) {
        perror("malloc task_kernel");
        mq_close(ind_mq);
        free(targ);
        pthread_mutex_lock(&task->lock);
        task->active = 0;
        pthread_mutex_unlock(&task->lock);
        pthread_exit(NULL);
    }
    memset(new_kernel->IPC, 0, sizeof(new_kernel->IPC));
    memset(new_kernel->cache_miss, 0, sizeof(new_kernel->cache_miss));
    memset(new_kernel->cache_reference, 0, sizeof(new_kernel->cache_reference));
    memset(new_kernel->throughput, 0, sizeof(new_kernel->throughput));
    memset(new_kernel->FLOPS, 0, sizeof(new_kernel->FLOPS));
    new_kernel->next = NULL;
    insert_task_kernel(task, new_kernel);
    printf("Task Handler: Added task_kernel to task '%s'\n", task->name);
    char buffer[MAX_MSG_SIZE + 1];
    ssize_t bytes_read;
    while (1) {
        memset(buffer, 0, sizeof(buffer));
        bytes_read = mq_receive(ind_mq, buffer, MAX_MSG_SIZE, NULL);
        if (bytes_read >= 0) {
            buffer[bytes_read] = '\0';
            if (strncmp(buffer, "PROFILE:", 8) == 0) {
                profiling_data pdata;
                if (parse_profiling_message(buffer, &pdata) == 5) {
                    pthread_mutex_lock(&task->lock);
                    if (task->profiles_received < NUM_CONFIGS) {
                        task->profiles[task->profiles_received] = pdata;
                        task->profiles_received++;
                        printf("Task '%s': Received profiling data (%d/%d) - thread_count=%d, throughput=%.2lf\n",
                               task->name, task->profiles_received, NUM_CONFIGS,
                               pdata.thread_count, pdata.throughput);
                        if (task->profiles_received == NUM_CONFIGS) {
                            task->profiling_complete = 1;
                            printf("Task '%s': Profiling complete.\n", task->name);
                            save_profiling_data(task);
                        }
                    }
                    pthread_mutex_unlock(&task->lock);
                } else {
                    printf("Task '%s': Invalid profiling message format: %s\n", task->name, buffer);
                }
            } else {
                printf("Task '%s': Received message: %s\n", task->name, buffer);
            }
        } else {
            perror("mq_receive individual");
            break;
        }
    }
    mq_close(ind_mq);
    free(targ);
    pthread_mutex_lock(&task->lock);
    task->active = 0;
    pthread_mutex_unlock(&task->lock);
    pthread_exit(NULL);
}

void brute_force_enum(omp_task **tasks, int n, int pos, int *current_comb, double *best_score, int *best_comb, double *indep_throughputs) {
    if (pos == n) {
        double total_score = 0.0;
        for (int i = 0; i < n; i++) {
            omp_task* task = tasks[i];
            int idx = current_comb[i];
            profiling_data pd = task->profiles[idx];
            double score = pd.flops - WEIGHT_THROUGHPUT*(indep_throughputs[i] - pd.throughput)
                           - WEIGHT_CACHE * pd.cache_miss_rate + WEIGHT_MEMBW * pd.mem_bw;
            total_score += score;
        }
        if (total_score > *best_score) {
            *best_score = total_score;
            for (int i = 0; i < n; i++) {
                best_comb[i] = current_comb[i];
            }
        }
        return;
    }
    for (int j = 0; j < tasks[pos]->profiles_received; j++) {
        current_comb[pos] = j;
        brute_force_enum(tasks, n, pos + 1, current_comb, best_score, best_comb, indep_throughputs);
    }
}

void perform_scheduling() {
    omp_task* sched_tasks[MAX_TASKS];
    int n = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        pthread_mutex_lock(&global_tasks[i].lock);
        if (global_tasks[i].active && global_tasks[i].profiling_complete && !global_tasks[i].scheduled) {
            sched_tasks[n++] = &global_tasks[i];
        }
        pthread_mutex_unlock(&global_tasks[i].lock);
    }
    if (n == 0) return;
    double indep_throughputs[MAX_TASKS];
    for (int i = 0; i < n; i++) {
        omp_task *task = sched_tasks[i];
        double best = -1.0;
        for (int j = 0; j < task->profiles_received; j++) {
            if (task->profiles[j].throughput > best) {
                best = task->profiles[j].throughput;
            }
        }
        indep_throughputs[i] = best;
    }
    int chosen_config[MAX_TASKS];
    double global_score = 0.0;
    int use_bruteforce = (n <= BRUTE_FORCE_THRESHOLD);
    if (use_bruteforce) {
        int current_comb[BRUTE_FORCE_THRESHOLD] = {0};
        int best_comb[BRUTE_FORCE_THRESHOLD] = {0};
        double best_global_score = -1e9;
        brute_force_enum(sched_tasks, n, 0, current_comb, &best_global_score, best_comb, indep_throughputs);
        for (int i = 0; i < n; i++) {
            chosen_config[i] = best_comb[i];
        }
        global_score = best_global_score;
    } else {
        for (int i = 0; i < n; i++) {
            omp_task *task = sched_tasks[i];
            double best_score = -1e9;
            int best_idx = 0;
            for (int j = 0; j < task->profiles_received; j++) {
                profiling_data pd = task->profiles[j];
                double score = pd.flops - WEIGHT_THROUGHPUT*(indep_throughputs[i] - pd.throughput)
                               - WEIGHT_CACHE * pd.cache_miss_rate + WEIGHT_MEMBW * pd.mem_bw;
                if (score > best_score) {
                    best_score = score;
                    best_idx = j;
                }
            }
            chosen_config[i] = best_idx;
            global_score += best_score;
        }
    }
    for (int i = 0; i < n; i++) {
        omp_task *task = sched_tasks[i];
        pthread_mutex_lock(&task->lock);
        task->optimal_thread_count = task->profiles[chosen_config[i]].thread_count;
        task->scheduled_core = i;
        task->scheduled = 1;
        pthread_mutex_unlock(&task->lock);
        char sched_msg[64];
        snprintf(sched_msg, sizeof(sched_msg), "SCHEDULE:%d,%d", task->optimal_thread_count, task->scheduled_core);
        if (mq_send(task->mq, sched_msg, strlen(sched_msg), 0) == -1) {
            perror("mq_send scheduling message");
        } else {
            printf("Scheduled task '%s' with %d threads on core %d (config index %d, global score=%.2lf)\n",
                   task->name, task->optimal_thread_count, task->scheduled_core, chosen_config[i], global_score);
        }
    }
}

void* scheduling_thread(void *arg) {
    (void)arg;
    while (1) {
        sleep(SCHEDULING_INTERVAL);
        printf("Scheduling thread: Evaluating tasks...\n");
        perform_scheduling();
    }
    pthread_exit(NULL);
}

int main(void) {
    printf("gsched daemon started.\n");
    load_profiling_data();
    for (int i = 0; i < MAX_TASKS; i++) {
        memset(global_tasks[i].name, 0, TASK_NAME_LEN);
        global_tasks[i].mq = (mqd_t)-1;
        global_tasks[i].kernel_list = NULL;
        global_tasks[i].active = 0;
        global_tasks[i].profiles_received = 0;
        global_tasks[i].profiling_complete = 0;
        global_tasks[i].optimal_thread_count = 0;
        global_tasks[i].scheduled_core = -1;
        global_tasks[i].scheduled = 0;
        pthread_mutex_init(&global_tasks[i].lock, NULL);
    }
    struct mq_attr gattr;
    gattr.mq_flags = 0;
    gattr.mq_maxmsg = 10;
    gattr.mq_msgsize = MAX_MSG_SIZE;
    gattr.mq_curmsgs = 0;
    mqd_t global_mqd = mq_open(GLOBAL_MQ_NAME, O_RDONLY | O_CREAT, 0644, &gattr);
    if (global_mqd == (mqd_t)-1) {
        perror("mq_open global");
        exit(EXIT_FAILURE);
    }
    printf("Listening on global mqueue '%s' for new OMP tasks...\n", GLOBAL_MQ_NAME);
    pthread_t sched_tid;
    if (pthread_create(&sched_tid, NULL, scheduling_thread, NULL) != 0) {
        perror("pthread_create scheduling_thread");
        exit(EXIT_FAILURE);
    }
    pthread_detach(sched_tid);
    char buffer[MAX_MSG_SIZE + 1];
    ssize_t bytes_read;
    while (1) {
        memset(buffer, 0, sizeof(buffer));
        bytes_read = mq_receive(global_mqd, buffer, MAX_MSG_SIZE, NULL);
        if (bytes_read >= 0) {
            buffer[bytes_read] = '\0';
            printf("Received new task notification: %s\n", buffer);
            omp_task *selected_task = NULL;
            int slotFound = 0;
            for (int i = 0; i < MAX_TASKS; i++) {
                int idx = (current_task_index + i) % MAX_TASKS;
                pthread_mutex_lock(&global_tasks[idx].lock);
                if (global_tasks[idx].active == 0) {
                    selected_task = &global_tasks[idx];
                    global_tasks[idx].active = 1;
                    global_tasks[idx].profiles_received = 0;
                    global_tasks[idx].profiling_complete = 0;
                    global_tasks[idx].scheduled = 0;
                    current_task_index = (idx + 1) % MAX_TASKS;
                    slotFound = 1;
                    pthread_mutex_unlock(&global_tasks[idx].lock);
                    break;
                }
                pthread_mutex_unlock(&global_tasks[idx].lock);
            }
            if (!slotFound) {
                fprintf(stderr, "No free task slot available.\n");
                continue;
            }
            task_thread_arg *targ = (task_thread_arg*) malloc(sizeof(task_thread_arg));
            if (!targ) {
                perror("malloc task_thread_arg");
                pthread_mutex_lock(&selected_task->lock);
                selected_task->active = 0;
                pthread_mutex_unlock(&selected_task->lock);
                continue;
            }
            targ->task = selected_task;
            strncpy(targ->task_name, buffer, TASK_NAME_LEN - 1);
            targ->task_name[TASK_NAME_LEN - 1] = '\0';
            pthread_t tid;
            if (pthread_create(&tid, NULL, task_handler, targ) != 0) {
                perror("pthread_create task_handler");
                free(targ);
                pthread_mutex_lock(&selected_task->lock);
                selected_task->active = 0;
                pthread_mutex_unlock(&selected_task->lock);
                continue;
            }
            pthread_detach(tid);
        } else {
            perror("mq_receive global");
            sleep(1);
        }
    }
    mq_close(global_mqd);
    return 0;
}
