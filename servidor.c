#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <pthread.h>
#include <string.h>
#include <time.h>   
#include "common.h"

#define DB_USERS "users.dat"

int global_id_counter = 1; // Contador global de clientes
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
    key_t client_key = ftok(".", 100 + client_id); // <--- CAMBIADO PARA SOPORTAR N CLIENTES
    int shmid = shmget(client_key, sizeof(shm_data), 0666);	
    if (shmid == -1) {
        perror("Error al conectar con SHM del cliente");
        pthread_exit(NULL);
    }
    
    shm_data *shm_ptr = (shm_data *)shmat(shmid, NULL, 0);
    printf(">> Hilo [TID: %lu] iniciado para atender al Cliente %d\n", (unsigned long)pthread_self(), client_id);

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
//------------------------------------------------------------------------------------------------------------------------------
            case OP_LOGIN: {
                char req_user[50], req_pass[65];
                sscanf(shm_ptr->payload, "%s %s", req_user, req_pass); // El cliente manda usuario y hash

                FILE *file = fopen(DB_USERS, "r");
                int success = 0;
                
                if (file) {
                    char linea[256];
                    char u_file[50], p_file[65];
                    // NUEVO: Leemos por renglón completo para ignorar las nuevas columnas de datos personales
                    while (fgets(linea, sizeof(linea), file)) {
                        if (sscanf(linea, "%s %s", u_file, p_file) >= 2) {
                            if (strcmp(req_user, u_file) == 0 && strcmp(req_pass, p_file) == 0) {
                                success = 1;
                                break;
                            }
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
            //---------------------------------------------------------------------------------------------------------------------------
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
//-------------------------------------------------------------------------------------------------------------------------------------
            case OP_REGISTER: {
                // NUEVO: Declaramos variables para las 6 columnas
                char req_user[50], req_pass[65], req_nom[50], req_ape[50], req_correo[100], req_tel[20];
                
                // Valores por defecto en caso de que un registro llegue incompleto
                strcpy(req_nom, "-"); strcpy(req_ape, "-"); strcpy(req_correo, "-"); strcpy(req_tel, "-");
                
                // Extraemos todo el paquete que nos mandó el nuevo asistente de Registro
                sscanf(shm_ptr->payload, "%s %s %s %s %s %s", req_user, req_pass, req_nom, req_ape, req_correo, req_tel);

                FILE *file = fopen(DB_USERS, "a");
                if (file) {
                    // Guardamos la fila completa en la base de datos
                    fprintf(file, "%s %s %s %s %s %s\n", req_user, req_pass, req_nom, req_ape, req_correo, req_tel);
                    fclose(file);
                    shm_ptr->status = 1;
                    strcpy(shm_ptr->respuesta, "Usuario registrado en el servidor.");
                    printf("   [+] Nuevo usuario registrado: %s (%s %s)\n", req_user, req_nom, req_ape);
                } else {
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "Error del servidor al registrar.");
                }
                break;
            }
            //--------------------------------------------------------------------------------------------------------------------
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
            //------------------------------------------------------------------------------------------------------------------------------
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
//-----------------------------------------------------------------------------------------------------------------------------------------
            case OP_GET_CATALOG: {
                // Lee el archivo del proveedor (ID, Nombre, Caducidad)
                FILE *file = fopen("catalogo.dat", "r");
                shm_ptr->respuesta[0] = '\0'; 
                
                if (file) {
                    char linea[256];
                    while (fgets(linea, sizeof(linea), file)) {
                        strncat(shm_ptr->respuesta, linea, sizeof(shm_ptr->respuesta) - strlen(shm_ptr->respuesta) - 1);
                    }
                    fclose(file);
                    shm_ptr->status = 1;
                    printf("   [INFO] Se envio el catalogo de proveedores al Cliente %d\n", client_id);
                } else {
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "Error: No se pudo abrir catalogo.dat\n");
                }
                break;
            }
//---------------------------------------------------BUY CART----------------------------------------------------------------------------
            case OP_BUY_CART: {
                char req_user[50];
                strcpy(req_user, shm_ptr->payload);
                
                char nombre_archivo[100];
                snprintf(nombre_archivo, sizeof(nombre_archivo), "carrito_%s.dat", req_user);
                
                FILE *f_cart = fopen(nombre_archivo, "r");
                if (!f_cart) {
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "Tu orden a proveedores esta vacia.");
                    break;
                }
                
                // --- NUEVO: ARCHIVO DE HISTORIAL DE COMPRAS CON FECHA ---
                FILE *f_compras = fopen("compras_diarias.dat", "a");
                time_t t = time(NULL);
                struct tm *tm_info = localtime(&t);
                char fecha_actual[20];
                // Generamos la fecha en formato YYYY-MM-DD
                strftime(fecha_actual, sizeof(fecha_actual), "%Y-%m-%d", tm_info);
                
                // Leemos línea por línea el pedido al proveedor
                char linea_cart[256];
                while (fgets(linea_cart, sizeof(linea_cart), f_cart)) {
                    char id_c[20], nom_c[100], cad_c[20];
                    int cant_c;
                    
                    if (sscanf(linea_cart, " %[^,] , %[^,] , %d , %s", id_c, nom_c, &cant_c, cad_c) == 4) {
                        
                        // Escribimos el registro en el historial para el Administrador
                        if (f_compras) {
                            fprintf(f_compras, "%s, %s, %s, %d\n", fecha_actual, req_user, nom_c, cant_c);
                        }
                        
                        // Lógica para sumar al inventario
                        FILE *f_art = fopen("articulos.dat", "r");
                        FILE *f_tmp = fopen("articulos.tmp", "w");
                        int producto_existente = 0;
                        
                        if (f_art && f_tmp) {
                            char linea_art[256];
                            while (fgets(linea_art, sizeof(linea_art), f_art)) {
                                char id_a[20], nom_a[100], cad_a[20];
                                int cant_a;
                                
                                if (sscanf(linea_art, " %[^,] , %[^,] , %d , %s", id_a, nom_a, &cant_a, cad_a) == 4) {
                                    if (strcmp(id_c, id_a) == 0) {
                                        cant_a += cant_c;
                                        fprintf(f_tmp, "%s, %s, %d, %s\n", id_a, nom_a, cant_a, cad_a); 
                                        producto_existente = 1;
                                    } else {
                                        fprintf(f_tmp, "%s", linea_art);
                                    }
                                }
                            }
                            fclose(f_art);
                            
                            if (!producto_existente) {
                                fprintf(f_tmp, "%s, %s, %d, %s\n", id_c, nom_c, cant_c, cad_c);
                            }
                            fclose(f_tmp);
                            
                            remove("articulos.dat");
                            rename("articulos.tmp", "articulos.dat");
                        } else {
                            if (f_art) fclose(f_art);
                            if (f_tmp) fclose(f_tmp);
                            FILE *f_new = fopen("articulos.dat", "a");
                            if (f_new) {
                                fprintf(f_new, "%s, %s, %d, %s\n", id_c, nom_c, cant_c, cad_c);
                                fclose(f_new);
                            }
                        }
                    }
                }
                fclose(f_cart);
                if (f_compras) fclose(f_compras); // Cerramos el archivo de historial
                
                // Vaciamos el carrito
                FILE *f_clear = fopen(nombre_archivo, "w");
                if (f_clear) fclose(f_clear);
                
                shm_ptr->status = 1;
                strcpy(shm_ptr->respuesta, "¡Pedido recibido! Inventario reabastecido.");
                printf("   [+] Cliente %d (%s) registro compra a proveedores. Inventario actualizado.\n", client_id, req_user);
                break;
            }
//----------------------------------------------------UPDATE PROFILE------------------------------------------------------------------------
            case OP_UPDATE_PROFILE: {
                char old_user[50], new_user[50], new_pass[65];
                char new_nom[50], new_ape[50], new_correo[100], new_tel[20];
                
                // Ahora desempacamos los 7 datos separados por el símbolo '|'
                char *t1 = strtok(shm_ptr->payload, "|");
                char *t2 = strtok(NULL, "|");
                char *t3 = strtok(NULL, "|");
                char *t4 = strtok(NULL, "|");
                char *t5 = strtok(NULL, "|");
                char *t6 = strtok(NULL, "|");
                char *t7 = strtok(NULL, "|");
                
                if (t1 && t2 && t3 && t4 && t5 && t6 && t7) {
                    strcpy(old_user, t1);
                    strcpy(new_user, t2);
                    strcpy(new_pass, t3);
                    strcpy(new_nom, t4);
                    strcpy(new_ape, t5);
                    strcpy(new_correo, t6);
                    strcpy(new_tel, t7);
                    
                    FILE *file = fopen(DB_USERS, "r");
                    FILE *temp = fopen("users.tmp", "w");
                    
                    if (file && temp) {
                        char linea[256];
                        while (fgets(linea, sizeof(linea), file)) {
                            char u_file[50], p_file[65], n_file[50], a_file[50], c_file[100], t_file[20];
                            
                            if (sscanf(linea, "%s %s %s %s %s %s", u_file, p_file, n_file, a_file, c_file, t_file) >= 2) {
                                if (strcmp(old_user, u_file) == 0) {
                                    // Sobreescribimos absolutamente toda la fila con los nuevos datos
                                    fprintf(temp, "%s %s %s %s %s %s\n", new_user, new_pass, new_nom, new_ape, new_correo, new_tel);
                                } else {
                                    fprintf(temp, "%s", linea); // Copiamos intactos a los demás
                                }
                            }
                        }
                        fclose(file);
                        fclose(temp);
                        
                        remove(DB_USERS);
                        rename("users.tmp", DB_USERS);
                        
                        // Renombrar el archivo de carrito por si cambió su nombre de usuario
                        char old_cart[100], new_cart[100];
                        snprintf(old_cart, sizeof(old_cart), "carrito_%s.dat", old_user);
                        snprintf(new_cart, sizeof(new_cart), "carrito_%s.dat", new_user);
                        rename(old_cart, new_cart);
                        
                        shm_ptr->status = 1;
                        strcpy(shm_ptr->respuesta, "Perfil actualizado correctamente.");
                        printf("   [*] Perfil modificado: %s ha actualizado todos sus datos.\n", old_user);
                    } else {
                        if (file) fclose(file);
                        if (temp) fclose(temp);
                        shm_ptr->status = 0;
                        strcpy(shm_ptr->respuesta, "Error al abrir base de datos en el servidor.");
                    }
                } else {
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "Datos de actualizacion incompletos o corruptos.");
                }
                break;
            }
            //---------------------------------------------------------------------------------------------------------------------------
            case OP_GET_USERS: {
                FILE *file = fopen(DB_USERS, "r");
                shm_ptr->respuesta[0] = '\0';
                
                if (file) {
                    char linea[256];
                    char u_file[50];
                    // NUEVO: Leemos por renglón y extraemos SOLO el primer dato (el usuario)
                    while (fgets(linea, sizeof(linea), file)) {
                        if (sscanf(linea, "%s", u_file) == 1) {
                            strncat(shm_ptr->respuesta, u_file, sizeof(shm_ptr->respuesta) - strlen(shm_ptr->respuesta) - 1);
                            strncat(shm_ptr->respuesta, "\n", sizeof(shm_ptr->respuesta) - strlen(shm_ptr->respuesta) - 1);
                        }
                    }
                    fclose(file);
                    shm_ptr->status = 1;
                    printf("   [INFO] Se envio la lista de usuarios al Administrador %d\n", client_id);
                } else {
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "No hay usuarios registrados o no existe la BD.\n");
                }
                break;
            }
//------------------------------------------------------------------------------------------------------------------------------------------
            case OP_DELETE_USER: {
                char req_user[50];
                strcpy(req_user, shm_ptr->payload);

                if (strcmp(req_user, "admin") == 0) {
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "Error de seguridad: No puedes eliminar al administrador maestro.");
                    break;
                }

                FILE *file = fopen(DB_USERS, "r");
                FILE *temp = fopen("users.tmp", "w");
                int deleted = 0;

                if (file && temp) {
                    char linea[256];
                    char u_file[50];
                    // NUEVO: Mismo principio, leemos el renglón y extraemos solo el usuario a borrar
                    while (fgets(linea, sizeof(linea), file)) {
                        if (sscanf(linea, "%s", u_file) == 1) {
                            if (strcmp(req_user, u_file) == 0) {
                                deleted = 1; // Lo ignoramos (lo borramos)
                            } else {
                                fprintf(temp, "%s", linea); // Copiamos toda la fila intacta
                            }
                        }
                    }
                    fclose(file);
                    fclose(temp);

                    remove(DB_USERS);
                    rename("users.tmp", DB_USERS);

                    char cart_file[100];
                    snprintf(cart_file, sizeof(cart_file), "carrito_%s.dat", req_user);
                    remove(cart_file);

                    if (deleted) {
                        shm_ptr->status = 1;
                        strcpy(shm_ptr->respuesta, "Usuario eliminado correctamente.");
                        printf("   [-] El Administrador elimino al usuario: %s\n", req_user);
                    } else {
                        shm_ptr->status = 0;
                        strcpy(shm_ptr->respuesta, "Error: El usuario no existe en la base de datos.");
                    }
                } else {
                    if (file) fclose(file);
                    if (temp) fclose(temp);
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "Error interno al abrir la BD del servidor.");
                }
                break;
            }
            //------------------------------------------------------------------------------------------------------------------------------
            case OP_ADD_CATALOG_ITEM: {
                char nuevo_nombre[100], nueva_caducidad[20];
                // El administrador solo manda el nombre y la fecha
                sscanf(shm_ptr->payload, "%s %s", nuevo_nombre, nueva_caducidad);

                int max_id = 99; // Nuestro ID base, para que los registros empiecen en 100
                FILE *file = fopen("catalogo.dat", "r");
                if (file) {
                    char linea[256];
                    int id_actual;
                    // Leemos línea por línea para encontrar el ID más grande
                    while (fgets(linea, sizeof(linea), file)) {
                        if (sscanf(linea, "%d", &id_actual) == 1) {
                            if (id_actual > max_id) {
                                max_id = id_actual;
                            }
                        }
                    }
                    fclose(file);
                }

                int nuevo_id = max_id + 1; // Auto-incremento
                FILE *f_append = fopen("catalogo.dat", "a");
                if (f_append) {
                    // Escribimos con el formato exacto de tu catálogo
                    fprintf(f_append, "%d, %s, %s\n", nuevo_id, nuevo_nombre, nueva_caducidad);
                    fclose(f_append);
                    shm_ptr->status = 1;
                    snprintf(shm_ptr->respuesta, sizeof(shm_ptr->respuesta), "Exito. Producto '%s' con ID: %d", nuevo_nombre, nuevo_id);
                    printf("   [+] Admin agrego producto al catalogo: ID %d, %s\n", nuevo_id, nuevo_nombre);
                } else {
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "Error al escribir en el catalogo del servidor.");
                }
                break;
            }
//--------------------------------------------------------------------------------------------------------------------------------
            case OP_DELETE_CATALOG_ITEM: {
                char req_id[20];
                strcpy(req_id, shm_ptr->payload); // El payload es solo el ID a borrar

                FILE *file = fopen("catalogo.dat", "r");
                FILE *temp = fopen("catalogo.tmp", "w");
                int deleted = 0;

                if (file && temp) {
                    char linea[256];
                    while (fgets(linea, sizeof(linea), file)) {
                        char id_a[20];
                        // Extraemos todo antes de la primera coma para compararlo con el ID pedido
                        if (sscanf(linea, " %[^,]", id_a) == 1) {
                            if (strcmp(req_id, id_a) == 0) {
                                deleted = 1; // Si es el ID, lo ignoramos (lo borramos)
                            } else {
                                fprintf(temp, "%s", linea); // Si no es, lo copiamos al archivo temporal
                            }
                        }
                    }
                    fclose(file);
                    fclose(temp);
                    
                    // Sustituimos la base de datos con la versión limpia
                    remove("catalogo.dat");
                    rename("catalogo.tmp", "catalogo.dat");

                    if (deleted) {
                        shm_ptr->status = 1;
                        strcpy(shm_ptr->respuesta, "Producto eliminado del catalogo.");
                        printf("   [-] Admin elimino producto del catalogo: ID %s\n", req_id);
                    } else {
                        shm_ptr->status = 0;
                        strcpy(shm_ptr->respuesta, "Error: Producto no encontrado.");
                    }
                } else {
                    if (file) fclose(file);
                    if (temp) fclose(temp);
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "Error al acceder al catalogo de la base de datos.");
                }
                break;
            }
//-------------------------------------------------------------------------------------------------------------------------------            
            case OP_CHECK_ALERTS: {
                FILE *file = fopen("articulos.dat", "r");
                shm_ptr->respuesta[0] = '\0';
                
                if (file) {
                    char linea[256];
                    char id[20], nombre[100], fecha[20];
                    int cantidad;
                    
                    // Obtener fecha actual del sistema
                    time_t t_actual = time(NULL);
                    int hay_alertas = 0;

                    while (fgets(linea, sizeof(linea), file)) {
                        // Leer el formato: ID, Nombre, Cantidad, YYYY-MM-DD
                        if (sscanf(linea, "%[^,], %[^,], %d, %s", id, nombre, &cantidad, fecha) == 4) {
                            struct tm tm_caducidad = {0};
                            int anio = 0, mes = 0, dia = 0;
                            
                            sscanf(fecha, "%d-%d-%d", &anio, &mes, &dia);
                            tm_caducidad.tm_year = anio - 1900;
                            tm_caducidad.tm_mon = mes - 1;
                            tm_caducidad.tm_mday = dia;

                            time_t t_caducidad = mktime(&tm_caducidad);
                            double diff_segundos = difftime(t_caducidad, t_actual);
                            int dias_restantes = (int)(diff_segundos / 86400);

                            // Simulación: Si faltan 15 días o menos, lanzar alerta
                            if (dias_restantes <= 15) { 
                                char alerta[200];
                                if (dias_restantes < 0) {
                                    snprintf(alerta, sizeof(alerta), "[MERMA] %s vencio hace %d dias!\n", nombre, -dias_restantes);
                                } else {
                                    snprintf(alerta, sizeof(alerta), "[ALERTA] %s caduca en %d dias.\n", nombre, dias_restantes);
                                }
                                strncat(shm_ptr->respuesta, alerta, sizeof(shm_ptr->respuesta) - strlen(shm_ptr->respuesta) - 1);
                                hay_alertas = 1;
                            }
                        }
                    }
                    fclose(file);

                    if (!hay_alertas) {
                        strcpy(shm_ptr->respuesta, "Inventario sano. No hay mermas proximas.\n");
                    }
                    shm_ptr->status = 1;
                    printf("   [INFO] Se enviaron las alertas predictivas al Cliente %d\n", client_id);
                } else {
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "Error al acceder a los articulos.\n");
                }
                break;
            }
            //------------------------------------------------------------------------------------------------------------------------
            case OP_GET_REPORTS: {
                char tipo_reporte[20];
                strcpy(tipo_reporte, shm_ptr->payload); // Puede ser "DIARIO", "SEMANAL" o "MENSUAL"

                FILE *f_compras = fopen("compras_diarias.dat", "r");
                shm_ptr->respuesta[0] = '\0';
                
                if (!f_compras) {
                    shm_ptr->status = 0;
                    strcpy(shm_ptr->respuesta, "No hay historial de compras registrado todavia.");
                    break;
                }

                time_t t_actual = time(NULL);
                char linea[256];
                int hay_datos = 0;
                int total_articulos = 0;

                // Encabezado del reporte
                strcat(shm_ptr->respuesta, "Fecha       | Usuario | Producto | Cant\n");
                strcat(shm_ptr->respuesta, "--------------------------------------------\n");

                while (fgets(linea, sizeof(linea), f_compras)) {
                    char fecha[20], user[50], prod[100];
                    int cant;
                    
                    // Leemos el formato: YYYY-MM-DD, Usuario, Producto, Cantidad
                    if (sscanf(linea, " %[^,] , %[^,] , %[^,] , %d", fecha, user, prod, &cant) == 4) {
                        int anio = 0, mes = 0, dia = 0;
                        if (sscanf(fecha, "%d-%d-%d", &anio, &mes, &dia) == 3) {
                            
                            // Convertir la fecha leida a tiempo del sistema
                            struct tm tm_compra = {0};
                            tm_compra.tm_year = anio - 1900;
                            tm_compra.tm_mon = mes - 1;
                            tm_compra.tm_mday = dia;
                            
                            time_t t_compra = mktime(&tm_compra);
                            double diff_segundos = difftime(t_actual, t_compra);
                            int diff_dias = (int)(diff_segundos / 86400);
                            
                            // Lógica de filtrado
                            int incluir = 0;
                            if (strcmp(tipo_reporte, "DIARIO") == 0 && diff_dias <= 1) incluir = 1;
                            else if (strcmp(tipo_reporte, "SEMANAL") == 0 && diff_dias <= 7) incluir = 1;
                            else if (strcmp(tipo_reporte, "MENSUAL") == 0 && diff_dias <= 30) incluir = 1;

                            if (incluir) {
                                char buffer_linea[150];
                                // Formateamos con espacios fijos (%-7s) para que la tabla quede derechita
                                snprintf(buffer_linea, sizeof(buffer_linea), "%s | %-7.7s | %-8.8s | %d\n", fecha, user, prod, cant);
                                
                                // Protegemos el buffer de la memoria compartida (max 1024 bytes)
                                if (strlen(shm_ptr->respuesta) + strlen(buffer_linea) < 900) {
                                    strcat(shm_ptr->respuesta, buffer_linea);
                                    total_articulos += cant;
                                    hay_datos = 1;
                                }
                            }
                        }
                    }
                }
                fclose(f_compras);

                if (!hay_datos) {
                    shm_ptr->status = 0;
                    snprintf(shm_ptr->respuesta, sizeof(shm_ptr->respuesta), "No hay compras para el reporte %s.", tipo_reporte);
                } else {
                    char buffer_total[100];
                    snprintf(buffer_total, sizeof(buffer_total), "--------------------------------------------\nTOTAL ARTICULOS: %d", total_articulos);
                    strcat(shm_ptr->respuesta, buffer_total);
                    shm_ptr->status = 1;
                    printf("   [INFO] Se envio el Reporte %s al Administrador %d\n", tipo_reporte, client_id);
                }
                break;
            }
            //-------------------------------------------------------------------------------------------------------------------
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
    // === LLAVES NUEVAS PARA EVITAR LA MEMORIA BASURA ===
    key_t base_key = ftok(".", 'X'); 
    key_t conn_key = ftok(".", 'Y'); 

    // Crear semáforo para nuevas conexiones (Handshake de 3 pasos)
    sem_conn_id = semget(conn_key, 3, IPC_CREAT | 0666);
    if (sem_conn_id == -1) {
        perror("Error: No se pudieron crear los semaforos de conexion");
        exit(1);
    }
    semctl(sem_conn_id, SEM_CONN, SETVAL, 0);
    semctl(sem_conn_id, SEM_ACK, SETVAL, 0);
    semctl(sem_conn_id, SEM_MUTEX, SETVAL, 1); // La puerta inicia libre

    // Crear arreglo de semáforos para los hilos privados
    semid = semget(base_key, MAX_CLIENTS * 2, IPC_CREAT | 0666);
    if (semid == -1) {
        perror("Error: No se pudo crear el arreglo de semaforos privados");
        exit(1);
    }
    for (int i = 0; i < MAX_CLIENTS * 2; i++) {
        semctl(semid, i, SETVAL, 0); 
    }

    printf("=========================================\n");
    printf("   Servidor de Inventarios Iniciado      \n");
    printf("   PID del Servidor Maestro: %d          \n", getpid());
    printf("=========================================\n");

    // Bucle principal: Escucha nuevas conexiones
    while (1) {
        sem_wait(sem_conn_id, SEM_CONN);

        // === BLINDAJE ANTI-SEGFAULT ===
        int common_shmid = shmget(base_key, sizeof(common_data), IPC_CREAT | 0666);
        if (common_shmid == -1) {
            perror("Error al leer la memoria publica");
            continue; 
        }
        common_data *common_ptr = (common_data *)shmat(common_shmid, NULL, 0);
        
        // NUEVO: Leemos el PID que nos dejó el cliente
        int pid_del_cliente = common_ptr->client_pid;
        
        int client_id = global_id_counter++; 
        common_ptr->client_id = client_id; 
        shmdt(common_ptr);

        sem_signal(sem_conn_id, SEM_ACK);

        // ACTUALIZADO: Imprimimos el ID y el PID. (El TID se imprimirá después en el hilo)
        printf("\n[*] Nueva conexion detectada. Se asigno el Hilo/Cliente %d (PID del proceso: %d)\n", client_id, pid_del_cliente);

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
