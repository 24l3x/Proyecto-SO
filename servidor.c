#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <pthread.h>
#include <string.h>
#include "common.h"

#define DB_USERS "users.dat"

int semid; // Arreglo de semáforos para sincronización Cliente-Hilo
int sem_conn_id; // Semáforo para el apretón de manos inicial

// Funciones auxiliares para simplificar el uso de semáforos
void sem_wait(int id, int num) {
    struct sembuf op = {num, -1, 0};
    semop(id, &op, 1);
}

void sem_signal(int id, int num) {
    struct sembuf op = {num, 1, 0};
    semop(id, &op, 1);
}

// Hilo dedicado a cada cliente (Memoria compartida privada)
void *handle_client(void *arg) {
    int client_id = *(int *)arg;
    free(arg);

    // Obtener la clave privada de este cliente
    key_t client_key = ftok(".", 'A' + client_id);
    int shmid = shmget(client_key, sizeof(shm_data), 0666);
    if (shmid == -1) {
        perror("Error al conectar con SHM del cliente");
        pthread_exit(NULL);
    }
    
    shm_data *shm_ptr = (shm_data *)shmat(shmid, NULL, 0);
    printf(">> Hilo iniciado para atender al Cliente %d\n", client_id);

    int sem_c2s = client_id * 2;     // Índice del semáforo: Cliente avisa al Servidor
    int sem_s2c = client_id * 2 + 1; // Índice del semáforo: Servidor avisa al Cliente

    while (1) {
        // 1. Esperar a que el cliente escriba una petición en la memoria
        sem_wait(semid, sem_c2s);

        if (shm_ptr->peticion == OP_EXIT) {
            printf("<< Cliente %d solicitó desconexión. Cerrando hilo.\n", client_id);
            break;
        }

        // Mostrar acción solicitada (Requisito de la rúbrica)
        printf("-- Acción del Cliente %d: Operación %d, Datos: %s\n", client_id, shm_ptr->peticion, shm_ptr->payload);

        switch (shm_ptr->peticion) {
            case OP_LOGIN: {
                char req_user[50], req_pass[65];
                sscanf(shm_ptr->payload, "%s %s", req_user, req_pass); // El cliente manda el hash

                FILE *file = fopen(DB_USERS, "r");
                int success = 0;
                
                if (file) {
                    char u_file[50], p_file[65];
                    while (fscanf(file, "%s %s", u_file, p_file) != EOF) {
                        if (strcmp(req_user, u_file) == 0 && strcmp(req_pass, p_file) == 0) {
                            success = 1;
                            break;
                        }
                    }
                    fclose(file);
                } else {
                    printf("   [!] Advertencia: Archivo de usuarios no existe aún.\n");
                }
                
                shm_ptr->status = success;
                strcpy(shm_ptr->respuesta, success ? "Login exitoso." : "Credenciales incorrectas.");
                break;    
            }
            case OP_GET_PRODUCTS: {
                FILE *file = fopen("articulos.dat", "r");
                shm_ptr->respuesta[0] = '\0'; // Limpiar la respuesta anterior
                
                if (file) {
                    char linea[256];
                    while (fgets(linea, sizeof(linea), file)) {
                        strncat(shm_ptr->respuesta, linea, sizeof(shm_ptr->respuesta) - strlen(shm_ptr->respuesta) - 1);
                    }
                    fclose(file);
                    shm_ptr->status = 1;
                    printf("   [INFO] Se enviaron los artículos al Cliente %d\n", client_id);
                } else {
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "Error: No se pudo abrir el catálogo de artículos.\n");
                    printf("   [!] Error al abrir articulos.dat\n");
                }
                break;
            }

            case OP_REGISTER: {
                char req_user[50], req_pass[65];
                sscanf(shm_ptr->payload, "%s %s", req_user, req_pass);

                FILE *file = fopen(DB_USERS, "a");
                if (file) {
                    fprintf(file, "%s %s\n", req_user, req_pass);
                    fclose(file);
                    shm_ptr->status = 1;
                    strcpy(shm_ptr->respuesta, "Usuario registrado en el servidor.");
                    printf("   [+] Nuevo usuario registrado: %s\n", req_user);
                } else {
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "Error del servidor al registrar.");
                }
                break;
            }
	    case OP_ADD_CART: {
                char req_user[50], req_producto[200];
                
                // Buscar el separador '|' en el payload
                char *separador = strchr(shm_ptr->payload, '|');
                if (separador != NULL) {
                    *separador = '\0'; // Cortamos la cadena en dos
                    strcpy(req_user, shm_ptr->payload);
                    strcpy(req_producto, separador + 1);

                    // Crear o abrir el archivo del carrito específico del usuario
                    char nombre_archivo[100];
                    snprintf(nombre_archivo, sizeof(nombre_archivo), "carrito_%s.dat", req_user);
                    
                    FILE *file = fopen(nombre_archivo, "a");
                    if (file) {
                        fprintf(file, "%s\n", req_producto);
                        fclose(file);
                        shm_ptr->status = 1;
                        strcpy(shm_ptr->respuesta, "Articulo agregado al carrito.");
                        printf("   [+] Producto agregado al carrito de %s: %s\n", req_user, req_producto);
                    } else {
                        shm_ptr->status = 0;
                        strcpy(shm_ptr->respuesta, "Error del servidor al guardar en el carrito.");
                    }
                } else {
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "Formato de peticion invalido.");
                }
                break;
            }
            case OP_GET_CART: {
                char req_user[50];
                strcpy(req_user, shm_ptr->payload);
                
                char nombre_archivo[100];
                snprintf(nombre_archivo, sizeof(nombre_archivo), "carrito_%s.dat", req_user);
                
                FILE *file = fopen(nombre_archivo, "r");
                shm_ptr->respuesta[0] = '\0';
                
                if (file) {
                    char linea[256];
                    while (fgets(linea, sizeof(linea), file)) {
                        strncat(shm_ptr->respuesta, linea, sizeof(shm_ptr->respuesta) - strlen(shm_ptr->respuesta) - 1);
                    }
                    fclose(file);
                    shm_ptr->status = 1;
                    printf("   [INFO] Se envio el carrito al Cliente %d (%s)\n", client_id, req_user);
                } else {
                    shm_ptr->status = 1; // Un carrito vacio no es un error critico, solo se reporta vacio
                    strcpy(shm_ptr->respuesta, "Tu carrito esta vacio.\n");
                    printf("   [INFO] El carrito de %s esta vacio.\n", req_user);
                }
                break;
            }

            case OP_BUY_CART: {
                char req_user[50];
                strcpy(req_user, shm_ptr->payload);
                
                char nombre_archivo[100];
                snprintf(nombre_archivo, sizeof(nombre_archivo), "carrito_%s.dat", req_user);
                
                FILE *f_cart = fopen(nombre_archivo, "r");
                if (!f_cart) {
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "No hay articulos que comprar.");
                    break;
                }
                
                // Requisito de la practica: Registrar en archivo de ventas
                FILE *f_sales = fopen("ventas_diarias.dat", "a");
                if (!f_sales) {
                    fclose(f_cart);
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "Error de servidor al procesar las ventas.");
                    break;
                }
                
                char linea[256];
                while (fgets(linea, sizeof(linea), f_cart)) {
                    fprintf(f_sales, "Comprador: %s | %s", req_user, linea);
                }
                fclose(f_cart);
                fclose(f_sales);
                
                // Vaciar el carrito truncando el archivo
                FILE *f_clear = fopen(nombre_archivo, "w");
                if (f_clear) fclose(f_clear);
                
                shm_ptr->status = 1;
                strcpy(shm_ptr->respuesta, "Compra procesada. ¡Gracias por tu compra!");
                printf("   [$] Cliente %d (%s) finalizo su compra con exito.\n", client_id, req_user);
                break;
            }

            case OP_UPDATE_PROFILE: {
                char old_user[50], new_user[50], new_pass[65];
                
                // El payload viene estructurado como: usuario_anterior|nuevo_usuario|nuevo_hash
                char *token1 = strtok(shm_ptr->payload, "|");
                char *token2 = strtok(NULL, "|");
                char *token3 = strtok(NULL, "|");
                
                if (token1 && token2 && token3) {
                    strcpy(old_user, token1);
                    strcpy(new_user, token2);
                    strcpy(new_pass, token3);
                    
                    FILE *file = fopen(DB_USERS, "r");
                    FILE *temp = fopen("users.tmp", "w");
                    int encontrado = 0;
                    
                    if (file && temp) {
                        char u_file[50], p_file[65];
                        while (fscanf(file, "%s %s", u_file, p_file) != EOF) {
                            if (strcmp(old_user, u_file) == 0) {
                                fprintf(temp, "%s %s\n", new_user, new_pass);
                                encontrado = 1;
                            } else {
                                fprintf(temp, "%s %s\n", u_file, p_file);
                            }
                        }
                        fclose(file);
                        fclose(temp);
                        
                        remove(DB_USERS);
                        rename("users.tmp", DB_USERS);
                        
                        // Opcional: Renombrar el archivo de su carrito si cambio su nombre de usuario
                        char old_cart[100], new_cart[100];
                        snprintf(old_cart, sizeof(old_cart), "carrito_%s.dat", old_user);
                        snprintf(new_cart, sizeof(new_cart), "carrito_%s.dat", new_user);
                        rename(old_cart, new_cart);
                        
                        shm_ptr->status = 1;
                        strcpy(shm_ptr->respuesta, "Perfil actualizado correctamente.");
                        printf("   [*] Perfil modificado: %s paso a ser %s\n", old_user, new_user);
                    } else {
                        if (file) fclose(file);
                        if (temp) fclose(temp);
                        shm_ptr->status = 0;
                        strcpy(shm_ptr->respuesta, "Error al abrir base de datos en el servidor.");
                    }
                } else {
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "Datos de actualizacion corruptos.");
                }
                break;
            }
            default:
                shm_ptr->status = 0;
                strcpy(shm_ptr->respuesta, "Operación en construcción.");
                break;
        }

        // 2. Avisar al cliente que ya terminamos de procesar y puede leer la respuesta
        sem_signal(semid, sem_s2c);
    }

    shmdt(shm_ptr);
    pthread_exit(NULL);
}

int main() {
    key_t base_key = ftok(".", 'S');
    key_t conn_key = ftok(".", 'C');

    // Crear semáforo para nuevas conexiones
    sem_conn_id = semget(conn_key, 1, IPC_CREAT | 0666);
    semctl(sem_conn_id, 0, SETVAL, 0);

    // Crear arreglo de semáforos para los hilos privados (2 por cliente)
    semid = semget(base_key, MAX_CLIENTS * 2, IPC_CREAT | 0666);
    for (int i = 0; i < MAX_CLIENTS * 2; i++) {
        semctl(semid, i, SETVAL, 0); 
    }

    printf("=========================================\n");
    printf("   Servidor de Inventarios Iniciado      \n");
    printf("=========================================\n");

    // Bucle principal: Escucha nuevas conexiones
    while (1) {
        // Espera a que un cliente nuevo mande el "apretón de manos"
        sem_wait(sem_conn_id, 0);

        int common_shmid = shmget(base_key, sizeof(common_data), 0666);
        common_data *common_ptr = (common_data *)shmat(common_shmid, NULL, 0);
        
        int client_id = common_ptr->client_id;
        shmdt(common_ptr);

        printf("\n[*] Nueva conexión detectada: Cliente %d\n", client_id);

        int *thread_arg = malloc(sizeof(int));
        *thread_arg = client_id;
        
        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_client, thread_arg) != 0) {
            perror("Error creando hilo");
            free(thread_arg);
        }
        pthread_detach(thread); 
    }

    return 0;
}
