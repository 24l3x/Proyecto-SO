#include <ncurses.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <openssl/sha.h>
#include "common.h"

// Variables globales para IPC
int semid;
int sem_c2s, sem_s2c;
shm_data *shm_ptr;

// Helpers de semáforos
void sem_wait(int id, int num) {
    struct sembuf op = {num, -1, 0};
    semop(id, &op, 1);
}

void sem_signal(int id, int num) {
    struct sembuf op = {num, 1, 0};
    semop(id, &op, 1);
}

void hash_password(const char *pass, char *out_hex) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char*)pass, strlen(pass), hash);
    for(int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(out_hex + (i * 2), "%02x", hash[i]);
    }
    out_hex[64] = '\0';
}

void draw_box(WINDOW *win, const char *title) {
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " %s ", title);
    wrefresh(win);
}

int enviar_peticion_servidor(op_type operacion, const char *payload) {
    shm_ptr->peticion = operacion;
    strncpy(shm_ptr->payload, payload, sizeof(shm_ptr->payload) - 1);
    
    sem_signal(semid, sem_c2s); 
    sem_wait(semid, sem_s2c);   
    
    return shm_ptr->status;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Uso: %s <id_admin: 1-%d>\n", argv[0], MAX_CLIENTS - 1);
        exit(1);
    }
    int my_id = atoi(argv[1]);

// ==== FASE 1: CONEXIÓN IPC ====
    key_t base_key = ftok(".", 'S');
    key_t conn_key = ftok(".", 'C');
    key_t my_key = ftok(".", 'A' + my_id); 

    int sem_conn_id = semget(conn_key, 1, 0666);
    semid = semget(base_key, MAX_CLIENTS * 2, 0666);
    
    int servidor_activo = 1;
    int shmid = -1; // <--- LA DECLARAMOS AQUÍ AFUERA PARA QUE SEA GLOBAL EN EL MAIN

    if (semid == -1 || sem_conn_id == -1) {
        servidor_activo = 0; 
    } else {
        // AQUI ABAJO LE QUITAMOS EL "int" PORQUE YA EXISTE ARRIBA
        shmid = shmget(my_key, sizeof(shm_data), IPC_CREAT | 0666); 
        shm_ptr = (shm_data *)shmat(shmid, NULL, 0);

        int common_shmid = shmget(base_key, sizeof(common_data), IPC_CREAT | 0666);
        common_data *common_ptr = (common_data *)shmat(common_shmid, NULL, 0);
        common_ptr->client_id = my_id;
        shmdt(common_ptr);
        
        sem_signal(sem_conn_id, 0); 
        sem_c2s = my_id * 2;
        sem_s2c = my_id * 2 + 1;
    }

    // ==== FASE 2: INTERFAZ NCURSES ====
    initscr();
    noecho();
    cbreak();
    curs_set(0); 

    int height = 14, width = 50; 
    int start_y = (LINES - height) / 2;
    int start_x = (COLS - width) / 2;
    WINDOW *win = newwin(height, width, start_y, start_x);
    keypad(win, TRUE); 
    
    char user[50], pass[50], payload_buffer[1024];
    int logged_in = 0;
    char usuario_actual[50] = "";

    while (1) {
        int opcion_seleccionada = 0;
        int ch;
        char title[64];
        
        snprintf(title, sizeof(title), logged_in ? "Administrador: %s" : "Panel de Control SGICPM", usuario_actual);

        const char *opciones[5]; 
        int num_opciones;
        
        if (!logged_in) {
            opciones[0] = "Iniciar Sesion";
            opciones[1] = "Salir";
            num_opciones = 2;
        } else {
            opciones[0] = "Modificar Catalogo";           
            opciones[1] = "Administrar Usuarios";     
            opciones[2] = "Reportes de Compras";         
            opciones[3] = "Cerrar Sesion";
            opciones[4] = "Salir";
            num_opciones = 5;
        }

        // --- DIBUJADO DEL MENÚ PRINCIPAL ---
        while (1) {
            werase(win);
            draw_box(win, title);
            
            if (servidor_activo) {
                wattron(win, A_BOLD); 
                mvwprintw(win, 1, width - 21, "[ Servidor: ONLINE ]");
                wattroff(win, A_BOLD);
            } else {
                wattron(win, A_STANDOUT); 
                mvwprintw(win, 1, width - 22, "[ Servidor: OFFLINE ]");
                wattroff(win, A_STANDOUT);
            }
            
            mvwprintw(win, 3, 2, "Seleccione una opcion:");
            
            for (int i = 0; i < num_opciones; i++) {
                if (i == opcion_seleccionada) wattron(win, A_REVERSE); 
                mvwprintw(win, 5 + i, 4, "%s", opciones[i]);
                if (i == opcion_seleccionada) wattroff(win, A_REVERSE); 
            }
            
            mvwprintw(win, height - 2, 3, "Usa flechas y ENTER");
            wrefresh(win);
            
            ch = wgetch(win);
            if (ch == KEY_UP) opcion_seleccionada = (opcion_seleccionada - 1 + num_opciones) % num_opciones;
            else if (ch == KEY_DOWN) opcion_seleccionada = (opcion_seleccionada + 1) % num_opciones;
            else if (ch == '\n' || ch == KEY_ENTER) {
                if (!servidor_activo && opcion_seleccionada != (num_opciones - 1)) {
                    mvwprintw(win, height - 3, 2, "¡No puedes hacer esto sin servidor!");
                    wrefresh(win);
                    wgetch(win); 
                    continue;    
                }
                break; 
            }
        }

        // --- LÓGICA DE ADMIN NO LOGUEADO ---
        if (!logged_in) {
            if (opcion_seleccionada == 1) break; 

            werase(win);
            draw_box(win, title);
            
            mvwprintw(win, 2, 2, "Usuario Admin: ");
            mvwprintw(win, 4, 2, "Password: ");
            mvwprintw(win, height - 2, 2, "[Deja vacio y da ENTER p/volver]");
            wrefresh(win);
            
            curs_set(1); 
            echo();
            mvwgetnstr(win, 2, 17, user, 49); 
            noecho();
            
            if (strlen(user) == 0) {
                curs_set(0);
                continue; 
            }
            
            int i = 0;
            wmove(win, 4, 12); 
            while ((ch = wgetch(win)) != '\n' && ch != '\r' && i < 49) { 
                if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
                    if (i > 0) {
                        i--;
                        int y, x;
                        getyx(win, y, x);
                        mvwaddch(win, y, x - 1, ' '); 
                        wmove(win, y, x - 1);
                        wrefresh(win);
                    }
                } else {
                    pass[i++] = ch;
                    waddch(win, '*'); 
                }
            }
            pass[i] = '\0';
            curs_set(0); 

            if (strlen(pass) == 0) {
                continue;
            }

            char hashed_pass[65];
            hash_password(pass, hashed_pass);
            snprintf(payload_buffer, sizeof(payload_buffer), "%s %s", user, hashed_pass);

            werase(win); 
            draw_box(win, title);
            
            if (opcion_seleccionada == 0) { // LOGIN
                // Validamos localmente que intente entrar como administrador
                if (strcmp(user, "admin") != 0) {
                    mvwprintw(win, 4, 2, "Error: Acceso exclusivo para administradores.");
                } else if (enviar_peticion_servidor(OP_LOGIN, payload_buffer)) {
                    logged_in = 1;
                    strncpy(usuario_actual, user, sizeof(usuario_actual));
                    mvwprintw(win, 4, 2, "ACCESO CONCEDIDO.");
                } else {
                    mvwprintw(win, 4, 2, "Error: Credenciales incorrectas.");
                }
            } 
            
            mvwprintw(win, 8, 2, "[Presiona cualquier tecla]");
            wrefresh(win);
            wgetch(win);

        } else {
            // --- LÓGICA DE ADMIN LOGUEADO ---
            if (opcion_seleccionada == 0) { // MODIFICAR CATALOGO
                int salir_cat = 0;
                int opcion_cat = 0;
                const char *opciones_cat[3] = {
                    "1. Ver / Eliminar Producto", 
                    "2. Agregar Nuevo Producto", 
                    "3. Volver al Menu Principal"
                };

                while (!salir_cat) {
                    werase(win);
                    draw_box(win, "Catalogo de Proveedores");
                    mvwprintw(win, 2, 2, "Seleccione una opcion:");
                    
                    for (int i = 0; i < 3; i++) {
                        if (i == opcion_cat) wattron(win, A_REVERSE);
                        mvwprintw(win, 4 + i, 4, "%s", opciones_cat[i]);
                        if (i == opcion_cat) wattroff(win, A_REVERSE);
                    }
                    
                    mvwprintw(win, height - 2, 3, "Usa flechas y ENTER");
                    wrefresh(win);
                    
                    int ch_cat = wgetch(win);
                    if (ch_cat == KEY_UP) opcion_cat = (opcion_cat - 1 + 3) % 3;
                    else if (ch_cat == KEY_DOWN) opcion_cat = (opcion_cat + 1) % 3;
                    else if (ch_cat == '\n' || ch_cat == KEY_ENTER) {
                        
                        if (opcion_cat == 0) { // 1. VER / ELIMINAR PRODUCTO
                            werase(win);
                            draw_box(win, "Cargando Catalogo...");
                            wrefresh(win);

                            // Reciclamos la instrucción del cliente para jalar la lista
                            if (enviar_peticion_servidor(OP_GET_CATALOG, "")) {
                                char productos[50][100];
                                int num_productos = 0;
                                
                                char *token = strtok(shm_ptr->respuesta, "\n");
                                while (token != NULL && num_productos < 50) {
                                    strncpy(productos[num_productos], token, 99);
                                    productos[num_productos][99] = '\0';
                                    num_productos++;
                                    token = strtok(NULL, "\n");
                                }

                                if (num_productos == 0) {
                                    mvwprintw(win, 4, 2, "El catalogo esta vacio.");
                                    mvwprintw(win, height - 2, 2, "[Presiona cualquier tecla]");
                                    wrefresh(win);
                                    wgetch(win);
                                } else {
                                    int prod_seleccionado = 0;
                                    int salir_lista = 0;
                                    while (!salir_lista) {
                                        werase(win);
                                        draw_box(win, "Eliminar Producto");
                                        mvwprintw(win, 2, 2, "(ENTER: Borrar | 'q': Volver)");
                                        mvwhline(win, 3, 2, ACS_HLINE, width - 4);
                                        
                                        for (int i = 0; i < num_productos && i < height - 6; i++) {
                                            if (i == prod_seleccionado) wattron(win, A_REVERSE);
                                            mvwprintw(win, 4 + i, 4, "%s", productos[i]);
                                            if (i == prod_seleccionado) wattroff(win, A_REVERSE);
                                        }
                                        wrefresh(win);
                                        
                                        int input_ch = wgetch(win);
                                        if (input_ch == KEY_UP && prod_seleccionado > 0) prod_seleccionado--;
                                        else if (input_ch == KEY_DOWN && prod_seleccionado < num_productos - 1) prod_seleccionado++;
                                        else if (input_ch == 'q' || input_ch == 'Q') salir_lista = 1;
                                        else if (input_ch == '\n' || input_ch == KEY_ENTER) {
                                            
                                            // Extraer solo el ID del producto seleccionado para mandar a borrarlo
                                            char id_borrar[20];
                                            sscanf(productos[prod_seleccionado], " %[^,]", id_borrar);

                                            werase(win);
                                            draw_box(win, "Atencion: Confirmar");
                                            mvwprintw(win, 4, 2, "Deseas borrar el ID: %s?", id_borrar);
                                            mvwprintw(win, 6, 2, "[ENTER] Si, borrar | [q] Cancelar");
                                            wrefresh(win);
                                            
                                            int conf = wgetch(win);
                                            if (conf == '\n' || conf == KEY_ENTER) {
                                                enviar_peticion_servidor(OP_DELETE_CATALOG_ITEM, id_borrar);
                                                
                                                werase(win);
                                                draw_box(win, "Resultado");
                                                mvwprintw(win, 4, 2, "%s", shm_ptr->respuesta);
                                                mvwprintw(win, height - 2, 2, "[Presiona cualquier tecla]");
                                                wrefresh(win);
                                                wgetch(win);
                                                salir_lista = 1; 
                                            }
                                        }
                                    }
                                }
                            } else {
                                mvwprintw(win, 4, 2, "Error al enlazar con el catalogo.");
                                wrefresh(win);
                                wgetch(win);
                            }
                        }
                        else if (opcion_cat == 1) { // 2. AGREGAR NUEVO PRODUCTO
                            char n_prod[50], f_cad[20];
                            werase(win);
                            draw_box(win, "Alta de Producto");
                            
                            // Solo pedimos Nombre y Caducidad
                            mvwprintw(win, 2, 2, "Nombre (sin espacios): ");
                            mvwprintw(win, 4, 2, "Caducidad (YYYY-MM-DD): ");
                            mvwprintw(win, height - 2, 2, "[Deja vacio y da ENTER p/volver]");
                            wrefresh(win);
                            
                            curs_set(1); 
                            echo();
                            mvwgetnstr(win, 2, 25, n_prod, 49); 
                            
                            if (strlen(n_prod) == 0) {
                                noecho(); curs_set(0);
                                continue; 
                            }
                            
                            mvwgetnstr(win, 4, 26, f_cad, 19);
                            noecho();
                            curs_set(0); 

                            if (strlen(f_cad) == 0) continue;
                            
                            char payload_cat[1024];
                            snprintf(payload_cat, sizeof(payload_cat), "%s %s", n_prod, f_cad);

                            werase(win); 
                            draw_box(win, "Alta de Producto");
                            
                            enviar_peticion_servidor(OP_ADD_CATALOG_ITEM, payload_cat);
                            mvwprintw(win, 4, 2, "%s", shm_ptr->respuesta);
                            
                            mvwprintw(win, height - 2, 2, "[Presiona cualquier tecla]");
                            wrefresh(win);
                            wgetch(win);
                        }
                        else if (opcion_cat == 2) { // 3. VOLVER AL MENÚ
                            salir_cat = 1;
                        }
                    }
                }
            }
            //--------------------------------------------------------------------------------------------------
            else if (opcion_seleccionada == 1) { // ADMINISTRAR USUARIOS
                int salir_usuarios = 0;
                int opcion_usr = 0;
                const char *opciones_usr[3] = {
                    "1. Ver / Eliminar Usuarios", 
                    "2. Crear Nuevo Usuario", 
                    "3. Volver al Menu Principal"
                };

                while (!salir_usuarios) {
                    werase(win);
                    draw_box(win, "Gestion de Usuarios");
                    mvwprintw(win, 2, 2, "Seleccione una opcion:");
                    
                    for (int i = 0; i < 3; i++) {
                        if (i == opcion_usr) wattron(win, A_REVERSE);
                        mvwprintw(win, 4 + i, 4, "%s", opciones_usr[i]);
                        if (i == opcion_usr) wattroff(win, A_REVERSE);
                    }
                    
                    mvwprintw(win, height - 2, 3, "Usa flechas y ENTER");
                    wrefresh(win);
                    
                    int ch_usr = wgetch(win);
                    if (ch_usr == KEY_UP) opcion_usr = (opcion_usr - 1 + 3) % 3;
                    else if (ch_usr == KEY_DOWN) opcion_usr = (opcion_usr + 1) % 3;
                    else if (ch_usr == '\n' || ch_usr == KEY_ENTER) {
                        
                        if (opcion_usr == 0) { // 1. VER Y ELIMINAR
                            werase(win);
                            draw_box(win, "Cargando Usuarios...");
                            wrefresh(win);

                            if (enviar_peticion_servidor(OP_GET_USERS, "")) {
                                char lista_usuarios[50][50];
                                int num_usuarios = 0;
                                
                                char *token = strtok(shm_ptr->respuesta, "\n");
                                while (token != NULL && num_usuarios < 50) {
                                    strncpy(lista_usuarios[num_usuarios], token, 49);
                                    lista_usuarios[num_usuarios][49] = '\0';
                                    num_usuarios++;
                                    token = strtok(NULL, "\n");
                                }

                                if (num_usuarios == 0) {
                                    mvwprintw(win, 4, 2, "No hay usuarios registrados.");
                                    mvwprintw(win, height - 2, 2, "[Presiona cualquier tecla]");
                                    wrefresh(win);
                                    wgetch(win);
                                } else {
                                    int usr_seleccionado = 0;
                                    int salir_lista = 0;
                                    while (!salir_lista) {
                                        werase(win);
                                        draw_box(win, "Lista de Empleados");
                                        mvwprintw(win, 2, 2, "(ENTER: Eliminar | 'q': Volver)");
                                        
                                        for (int i = 0; i < num_usuarios && i < height - 5; i++) {
                                            if (i == usr_seleccionado) wattron(win, A_REVERSE);
                                            mvwprintw(win, 4 + i, 4, "- %s", lista_usuarios[i]);
                                            if (i == usr_seleccionado) wattroff(win, A_REVERSE);
                                        }
                                        wrefresh(win);
                                        
                                        int input_ch = wgetch(win);
                                        if (input_ch == KEY_UP && usr_seleccionado > 0) usr_seleccionado--;
                                        else if (input_ch == KEY_DOWN && usr_seleccionado < num_usuarios - 1) usr_seleccionado++;
                                        else if (input_ch == 'q' || input_ch == 'Q') salir_lista = 1;
                                        else if (input_ch == '\n' || input_ch == KEY_ENTER) {
                                            
                                            // Pestaña de confirmación antes de eliminar
                                            werase(win);
                                            draw_box(win, "Atencion: Confirmar");
                                            mvwprintw(win, 4, 2, "Deseas borrar a: %s?", lista_usuarios[usr_seleccionado]);
                                            mvwprintw(win, 6, 2, "[ENTER] Si, borrar | [q] Cancelar");
                                            wrefresh(win);
                                            
                                            int conf = wgetch(win);
                                            if (conf == '\n' || conf == KEY_ENTER) {
                                                enviar_peticion_servidor(OP_DELETE_USER, lista_usuarios[usr_seleccionado]);
                                                
                                                werase(win);
                                                draw_box(win, "Resultado");
                                                mvwprintw(win, 4, 2, "%s", shm_ptr->respuesta);
                                                mvwprintw(win, height - 2, 2, "[Presiona cualquier tecla]");
                                                wrefresh(win);
                                                wgetch(win);
                                                salir_lista = 1; // Lo forzamos a salir para que la lista se refresque al entrar
                                            }
                                        }
                                    }
                                }
                            } else {
                                mvwprintw(win, 4, 2, "Error al enlazar con la BD.");
                                wrefresh(win);
                                wgetch(win);
                            }
                        }
                        else if (opcion_usr == 1) { // 2. CREAR NUEVO USUARIO
                            char nuevo_u[50], nuevo_p[50];
                            werase(win);
                            draw_box(win, "Alta de Nuevo Empleado");
                            
                            mvwprintw(win, 2, 2, "Usuario: ");
                            mvwprintw(win, 4, 2, "Password: ");
                            mvwprintw(win, height - 2, 2, "[Deja vacio y da ENTER p/volver]");
                            wrefresh(win);
                            
                            curs_set(1); 
                            echo();
                            mvwgetnstr(win, 2, 11, nuevo_u, 49); 
                            noecho();
                            
                            if (strlen(nuevo_u) == 0) {
                                curs_set(0);
                                continue; 
                            }
                            
                            int i = 0;
                            int ch_pass;
                            wmove(win, 4, 12); 
                            while ((ch_pass = wgetch(win)) != '\n' && ch_pass != '\r' && i < 49) { 
                                if (ch_pass == KEY_BACKSPACE || ch_pass == 127 || ch_pass == '\b') {
                                    if (i > 0) {
                                        i--;
                                        int y, x;
                                        getyx(win, y, x);
                                        mvwaddch(win, y, x - 1, ' '); 
                                        wmove(win, y, x - 1);
                                        wrefresh(win);
                                    }
                                } else {
                                    nuevo_p[i++] = ch_pass;
                                    waddch(win, '*'); 
                                }
                            }
                            nuevo_p[i] = '\0';
                            curs_set(0); 

                            if (strlen(nuevo_p) == 0) continue;

                            char hashed_p[65];
                            hash_password(nuevo_p, hashed_p);
                            
                            char payload_reg[1024];
                            snprintf(payload_reg, sizeof(payload_reg), "%s %s", nuevo_u, hashed_p);

                            werase(win); 
                            draw_box(win, "Alta de Nuevo Empleado");
                            
                            // Reusamos la instruccion OP_REGISTER que ya existía en el servidor
                            enviar_peticion_servidor(OP_REGISTER, payload_reg);
                            mvwprintw(win, 4, 2, "%s", shm_ptr->respuesta);
                            
                            mvwprintw(win, height - 2, 2, "[Presiona cualquier tecla]");
                            wrefresh(win);
                            wgetch(win);
                        }
                        else if (opcion_usr == 2) { // 3. VOLVER
                            salir_usuarios = 1;
                        }
                    }
                }
            }
            //--------------------------------------------------------------------------------------------------------
            else if (opcion_seleccionada == 2) { // REPORTES DE COMPRAS
                int salir_rep = 0;
                int opcion_rep = 0;
                const char *opciones_rep[4] = {
                    "1. Reporte Diario (Hoy)", 
                    "2. Reporte Semanal (Ultimos 7 dias)", 
                    "3. Reporte Mensual (Ultimos 30 dias)",
                    "4. Volver al Menu Principal"
                };

                while (!salir_rep) {
                    werase(win);
                    draw_box(win, "Reportes Financieros y de Compras");
                    mvwprintw(win, 2, 2, "Seleccione un periodo:");
                    
                    for (int i = 0; i < 4; i++) {
                        if (i == opcion_rep) wattron(win, A_REVERSE);
                        mvwprintw(win, 4 + i, 4, "%s", opciones_rep[i]);
                        if (i == opcion_rep) wattroff(win, A_REVERSE);
                    }
                    
                    mvwprintw(win, height - 2, 3, "Usa flechas y ENTER");
                    wrefresh(win);
                    
                    int ch_rep = wgetch(win);
                    if (ch_rep == KEY_UP) opcion_rep = (opcion_rep - 1 + 4) % 4;
                    else if (ch_rep == KEY_DOWN) opcion_rep = (opcion_rep + 1) % 4;
                    else if (ch_rep == '\n' || ch_rep == KEY_ENTER) {
                        
                        if (opcion_rep >= 0 && opcion_rep <= 2) { 
                            char tipo_peticion[20];
                            if (opcion_rep == 0) strcpy(tipo_peticion, "DIARIO");
                            else if (opcion_rep == 1) strcpy(tipo_peticion, "SEMANAL");
                            else strcpy(tipo_peticion, "MENSUAL");

                            werase(win);
                            draw_box(win, "Generando Reporte...");
                            wrefresh(win);

                            enviar_peticion_servidor(OP_GET_REPORTS, tipo_peticion);
                            
                            // Mostrar la tabla generada por el servidor
                            werase(win);
                            char titulo_rep[50];
                            snprintf(titulo_rep, sizeof(titulo_rep), "Reporte %s", tipo_peticion);
                            draw_box(win, titulo_rep);
                            
                            int y = 2;
                            char *token = strtok(shm_ptr->respuesta, "\n");
                            while (token != NULL && y < height - 2) {
                                mvwprintw(win, y++, 2, "%s", token);
                                token = strtok(NULL, "\n"); // Ir cortando y pintando por renglón
                            }
                            
                            mvwprintw(win, height - 2, 2, "[Presiona cualquier tecla para volver]");
                            wrefresh(win);
                            wgetch(win);
                        }
                        else if (opcion_rep == 3) { // 4. VOLVER
                            salir_rep = 1;
                        }
                    }
                }
            }
            //---------------------------------------------------------------------------------------------------
            else if (opcion_seleccionada == 3) { // Cerrar Sesión
                logged_in = 0;
                usuario_actual[0] = '\0'; 
                werase(win);
                draw_box(win, "Panel de Control SGICPM");
                mvwprintw(win, 4, 2, "Sesion cerrada de forma segura.");
                mvwprintw(win, 8, 2, "[Presiona cualquier tecla]");
                wrefresh(win);
                wgetch(win);
            }
            //--------------------------------------------------------------------------------------------------------
            else if (opcion_seleccionada == 4) { // Salir
                break; 
            }
        }
    }

    // ==== FASE 3: LIMPIEZA Y CIERRE ====
    if (servidor_activo) {
        enviar_peticion_servidor(OP_EXIT, "");
        shmdt(shm_ptr);
        shmctl(shmid, IPC_RMID, NULL);
    }
    
    delwin(win);
    endwin();
    
    return 0;
}
