#include <stdio.h>
#include <stdlib.h>
#include <sthread.h>

#define N_CAIXAS   2
#define N_CLIENTES 6

static sthread_mon_t monitor[N_CAIXAS];
static int clientes_na_fila[N_CAIXAS] = {0, 0};

void Atender(int tempo) {
    int i;
    for (i = 0; i < tempo * 1000; i++);
}

void SerAtendido(int tempo) {
    int i;
    for (i = 0; i < tempo * 1000; i++);
}

int EscolherFila() {
    if (clientes_na_fila[0] <= clientes_na_fila[1])
        return 0;
    return 1;
}

void *cliente(void *arg) {
    int id = (int)arg;
    int fila;
    int tempo = 3;

    fila = EscolherFila();
    printf("Cliente %d escolheu fila %d\n", id, fila);

    sthread_monitor_enter(monitor[fila]);
    clientes_na_fila[fila]++;
    sthread_monitor_signal(monitor[fila]);
    sthread_monitor_wait(monitor[fila]);
    SerAtendido(tempo);
    printf("Cliente %d foi atendido na fila %d\n", id, fila);
    sthread_monitor_exit(monitor[fila]);

    return NULL;
}

void *empregado(void *arg) {
    int fila = (int)arg;
    int outra_fila = 1 - fila;

    printf("Empregado %d arrancou\n", fila);

    while (1) {
        sthread_monitor_enter(monitor[fila]);

        while (clientes_na_fila[fila] == 0 &&
               clientes_na_fila[outra_fila] == 0) {
            printf("Empregado %d bloqueia-se\n", fila);
            sthread_monitor_wait(monitor[fila]);
        }

        if (clientes_na_fila[fila] > 0) {
            clientes_na_fila[fila]--;
            printf("Empregado %d atende cliente da fila %d\n", fila, fila);
            sthread_monitor_signal(monitor[fila]);
        } else {
    clientes_na_fila[outra_fila]--;
    printf("Empregado %d atende cliente da fila %d\n", fila, outra_fila);
    sthread_monitor_exit(monitor[fila]);
    sthread_monitor_enter(monitor[outra_fila]);
    sthread_monitor_signal(monitor[outra_fila]);
    sthread_monitor_exit(monitor[outra_fila]);
    sthread_monitor_enter(monitor[fila]);
}

        Atender(3);
        sthread_monitor_exit(monitor[fila]);
        sthread_yield();
    }
    return NULL;
}

int main(int argc, char **argv) {
    int i;

    printf("=== Supermercado ===\n");
    sthread_init();

    for (i = 0; i < N_CAIXAS; i++)
        monitor[i] = sthread_monitor_init();

    for (i = 0; i < N_CAIXAS; i++)
        sthread_create(empregado, (void*)i, 5);

    for (i = 0; i < N_CLIENTES; i++) {
        sthread_create(cliente, (void*)i, 5);
        sthread_yield();
    }

    for (i = 0; i < N_CLIENTES * 10; i++)
        sthread_yield();

    printf("=== Fim ===\n");
    return 0;
}
