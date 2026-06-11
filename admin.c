#include <ncurses.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <openssl/sha.h>
#include <ctype.h> 
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
    shm_ptr->payload[sizeof(shm_ptr->payload) - 1] = '\0';

    sem_signal(semid, sem_c2s); 
    sem_wait(semid, sem_s2c);   

    return shm_ptr->status;
}

// Función para validar la contraseña segura
int validar_password(const char *pwd) {
    int tiene_mayus = 0, tiene_minus = 0, tiene_num = 0, tiene_esp = 0;
    for (int i = 0; pwd[i] != '\0'; i++) {
        if (isupper(pwd[i])) tiene_mayus = 1;
        else if (islower(pwd[i])) tiene_minus = 1;
        else if (isdigit(pwd[i])) tiene_num = 1;
        else if (ispunct(pwd[i])) tiene_esp = 1; 
    }
    return (tiene_mayus && tiene_minus && tiene_num && tiene_esp);
}

int main(int argc, char *argv[]) {
    int my_id = 0; 
    int servidor_activo = 1; // <--- AQUÍ REGRESA LA VARIABLE QUE BORRAMOS
    int shmid = -1;

    // ==== FASE 1: CONEXIÓN IPC DINÁMICA ====
    key_t base_key = ftok(".", 'X');
    key_t conn_key = ftok(".", 'Y');

    // Intentamos obtener los semáforos sin crearlos
    int sem_conn_id = semget(conn_key, 3, 0666); 
    semid = semget(base_key, MAX_CLIENTS * 2, 0666);
    
    if (semid == -1 || sem_conn_id == -1) {
        servidor_activo = 0; // El servidor está apagado, pero dejamos que abra el menú offline
    } else {
        // --- NUEVO APRETÓN DE MANOS (HANDSHAKE) ---
        sem_wait(sem_conn_id, SEM_MUTEX); 
        
        // NUEVO: Abrimos la memoria y dejamos nuestro PID
        int common_shmid = shmget(base_key, sizeof(common_data), IPC_CREAT | 0666);
        common_data *common_ptr = (common_data *)shmat(common_shmid, NULL, 0);
        common_ptr->client_pid = getpid(); // Dejamos nuestra credencial (PID)
        
        sem_signal(sem_conn_id, SEM_CONN); 
        sem_wait(sem_conn_id, SEM_ACK);    

        my_id = common_ptr->client_id;     
        shmdt(common_ptr);                 

        sem_signal(sem_conn_id, SEM_MUTEX); 
        // ------------------------------------------

        key_t my_key = ftok(".", 100 + my_id); 
        shmid = shmget(my_key, sizeof(shm_data), IPC_CREAT | 0666); // <-- Tu variable intacta
        shm_ptr = (shm_data *)shmat(shmid, NULL, 0);

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
                                                salir_lista = 1; 
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
                        else if (opcion_usr == 1) { // 2. CREAR NUEVO USUARIO (Con Asistente de 6 Pasos)
                            char reg_user[50]="", reg_pass[50]="", reg_nom[50]="", reg_ape[50]="", reg_correo[100]="", reg_tel[20]="";
                            int paso = 0;
                            int salir_registro = 0;

                            while (paso < 6 && !salir_registro) {
                                werase(win);
                                draw_box(win, "Alta de Nuevo Empleado");
                                mvwprintw(win, 2, 2, "1. Usuario: %s", reg_user);
                                mvwprintw(win, 3, 2, "2. Password: %s", (paso > 1) ? "********" : "");
                                mvwprintw(win, 4, 2, "3. Nombre: %s", reg_nom);
                                mvwprintw(win, 5, 2, "4. Apellido: %s", reg_ape);
                                mvwprintw(win, 6, 2, "5. Correo: %s", reg_correo);
                                mvwprintw(win, 7, 2, "6. Telefono: %s", reg_tel);
                                mvwprintw(win, height - 2, 2, "[Deja vacio y da ENTER p/volver]");
                                wrefresh(win);

                                if (paso == 0) { // Validar Usuario
                                    curs_set(1); echo();
                                    mvwgetnstr(win, 2, 14, reg_user, 49);
                                    noecho(); curs_set(0);
                                    if(strlen(reg_user) == 0) { salir_registro = 1; break; }
                                    paso++;
                                }
                                else if (paso == 1) { // Validar Password
                                    int i = 0; int ch_pass;
                                    wmove(win, 3, 15);
                                    curs_set(1);
                                    while ((ch_pass = wgetch(win)) != '\n' && ch_pass != '\r' && i < 49) {
                                        if (ch_pass == KEY_BACKSPACE || ch_pass == 127 || ch_pass == '\b') {
                                            if (i > 0) {
                                                i--; int y, x; getyx(win, y, x);
                                                mvwaddch(win, y, x - 1, ' ');
                                                wmove(win, y, x - 1); wrefresh(win);
                                            }
                                        } else {
                                            reg_pass[i++] = ch_pass;
                                            waddch(win, '*');
                                        }
                                    }
                                    reg_pass[i] = '\0';
                                    curs_set(0);

                                    if(strlen(reg_pass) == 0) { salir_registro = 1; break; }

                                    if (!validar_password(reg_pass)) {
                                        werase(win); draw_box(win, "Error en Password");
                                        wattron(win, A_STANDOUT);
                                        mvwprintw(win, 4, 2, " La contrasena DEBE contener: ");
                                        wattroff(win, A_STANDOUT);
                                        mvwprintw(win, 6, 2, "- 1 Mayuscula, 1 Minuscula");
                                        mvwprintw(win, 7, 2, "- 1 Numero, 1 Simbolo especial");
                                        mvwprintw(win, height - 2, 2, "[Presiona ENTER para corregir]");
                                        wrefresh(win); wgetch(win);
                                    } else {
                                        paso++;
                                    }
                                }
                                else if (paso == 2) { // Validar Nombre
                                    curs_set(1); echo();
                                    mvwgetnstr(win, 4, 13, reg_nom, 49);
                                    noecho(); curs_set(0);
                                    if(strlen(reg_nom) == 0) { salir_registro = 1; break; }
                                    paso++;
                                }
                                else if (paso == 3) { // Validar Apellido
                                    curs_set(1); echo();
                                    mvwgetnstr(win, 5, 15, reg_ape, 49);
                                    noecho(); curs_set(0);
                                    if(strlen(reg_ape) == 0) { salir_registro = 1; break; }
                                    paso++;
                                }
                                else if (paso == 4) { // Validar Correo
                                    curs_set(1); echo();
                                    mvwgetnstr(win, 6, 13, reg_correo, 99);
                                    noecho(); curs_set(0);
                                    if(strlen(reg_correo) == 0) { salir_registro = 1; break; }

                                    if (strchr(reg_correo, '@') == NULL) {
                                        werase(win); draw_box(win, "Error en Correo");
                                        wattron(win, A_STANDOUT);
                                        mvwprintw(win, 5, 2, " Formato Invalido ");
                                        wattroff(win, A_STANDOUT);
                                        mvwprintw(win, 7, 2, "El correo debe contener un '@'.");
                                        mvwprintw(win, height - 2, 2, "[Presiona ENTER para corregir]");
                                        wrefresh(win); wgetch(win);
                                    } else {
                                        paso++;
                                    }
                                }
                                else if (paso == 5) { // Validar Telefono
                                    curs_set(1); echo();
                                    mvwgetnstr(win, 7, 15, reg_tel, 19);
                                    noecho(); curs_set(0);
                                    if(strlen(reg_tel) == 0) { salir_registro = 1; break; }

                                    int telefono_valido = 1;
                                    if (strlen(reg_tel) != 10) telefono_valido = 0;
                                    for (int k = 0; k < strlen(reg_tel); k++) {
                                        if (!isdigit(reg_tel[k])) telefono_valido = 0;
                                    }

                                    if (!telefono_valido) {
                                        werase(win); draw_box(win, "Error en Telefono");
                                        wattron(win, A_STANDOUT);
                                        mvwprintw(win, 4, 2, " Numero Invalido ");
                                        wattroff(win, A_STANDOUT);
                                        mvwprintw(win, 6, 2, "El telefono debe tener exactamente");
                                        mvwprintw(win, 7, 2, "10 digitos (Solo numeros).");
                                        mvwprintw(win, height - 2, 2, "[Presiona ENTER para corregir]");
                                        wrefresh(win); wgetch(win);
                                    } else {
                                        paso++;
                                    }
                                }
                            }

                            // Si terminó los 6 pasos sin salirse
                            if (!salir_registro && paso == 6) {
                                char hashed_pass[65];
                                hash_password(reg_pass, hashed_pass);
                                
                                char payload_reg[1024];
                                snprintf(payload_reg, sizeof(payload_reg), "%s %s %s %s %s %s", 
                                         reg_user, hashed_pass, reg_nom, reg_ape, reg_correo, reg_tel);

                                werase(win); draw_box(win, "Alta de Nuevo Empleado");
                                enviar_peticion_servidor(OP_REGISTER, payload_reg);
                                mvwprintw(win, 4, 2, "%s", shm_ptr->respuesta);
                                mvwprintw(win, height - 2, 2, "[Presiona cualquier tecla]");
                                wrefresh(win); wgetch(win);
                            }
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
