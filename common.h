#ifndef COMMON_H
#define COMMON_H

#define MAX_CLIENTS 5
#define SHM_SIZE 2048

// Tipos de peticiones que el cliente o admin le pueden hacer al servidor
typedef enum {
    // --- Operaciones del Cliente ---
    OP_LOGIN,
    OP_REGISTER,
    OP_GET_PRODUCTS,
    OP_GET_CATALOG,
    OP_ADD_CART,
    OP_GET_CART,         
    OP_BUY_CART,         
    OP_UPDATE_PROFILE,   
    OP_CHECK_ALERTS,     
    
    // --- Operaciones del Administrador ---
    OP_GET_USERS,           // Listar todos los usuarios del sistema
    OP_DELETE_USER,         // Eliminar a un usuario
    OP_ADD_CATALOG_ITEM,    // Agregar un nuevo producto al catálogo del proveedor
    OP_DELETE_CATALOG_ITEM, // Eliminar un producto del catálogo del proveedor
    OP_GET_REPORTS,         // Generar y ver los reportes de ventas
    
    OP_EXIT
} op_type;

// Estructura de la memoria compartida privada (Comunicación Cliente/Admin <-> Hilo)
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
