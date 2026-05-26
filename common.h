#ifndef COMMON_H
#define COMMON_H

#define MAX_CLIENTS 5
#define SHM_SIZE 2048

// Tipos de peticiones que el cliente le puede hacer al servidor
typedef enum {
    OP_LOGIN,
    OP_REGISTER,
    OP_GET_PRODUCTS,
    OP_ADD_CART,
    OP_GET_CART,         // <-- Asegúrate de que esta esté aquí
    OP_BUY_CART,         
    OP_UPDATE_PROFILE,   // <-- Y esta también
    OP_EXIT
} op_type;

// Estructura de la memoria compartida privada (Comunicación Cliente <-> Hilo)
typedef struct {
    op_type peticion;       
    char payload[1024];     
    char respuesta[1024];   
    int status;             
} shm_data;

// Estructura para el "apretón de manos" inicial (Conexión)
typedef struct {
    int client_id;
    int shmid;
} common_data;

#endif
