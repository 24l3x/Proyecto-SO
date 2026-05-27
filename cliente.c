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

// Función que empaqueta la comunicación con el servidor
int enviar_peticion_servidor(op_type operacion, const char *payload) {
    shm_ptr->peticion = operacion;
    strncpy(shm_ptr->payload, payload, sizeof(shm_ptr->payload) - 1);
    
    sem_signal(semid, sem_c2s); // Avisar al servidor
    sem_wait(semid, sem_s2c);   // Esperar respuesta
    
    return shm_ptr->status;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Uso: %s <id_cliente: 1-%d>\n", argv[0], MAX_CLIENTS - 1);
        exit(1);
    }
    int my_id = atoi(argv[1]);

    // ==== FASE 1: CONEXIÓN IPC ====
    key_t base_key = ftok(".", 'S');
    key_t conn_key = ftok(".", 'C');
    key_t my_key = ftok(".", 'A' + my_id); 

    // Intentamos obtener los semáforos sin crearlos (0666)
    int sem_conn_id = semget(conn_key, 1, 0666);
    semid = semget(base_key, MAX_CLIENTS * 2, 0666);
    
    // Si los semáforos no existen, el servidor está apagado
    if (semid == -1 || sem_conn_id == -1) {
        printf("El servidor está apagado por lo que no podemos iniciar el cliente\n");
        exit(1);
    }

    // Apretón de manos con el servidor
    int shmid = shmget(my_key, sizeof(shm_data), IPC_CREAT | 0666);
    shm_ptr = (shm_data *)shmat(shmid, NULL, 0);

    int common_shmid = shmget(base_key, sizeof(common_data), IPC_CREAT | 0666);
    common_data *common_ptr = (common_data *)shmat(common_shmid, NULL, 0);
    common_ptr->client_id = my_id;
    shmdt(common_ptr);
    
    sem_signal(sem_conn_id, 0); // Despertar al hilo principal del servidor

    sem_c2s = my_id * 2;
    sem_s2c = my_id * 2 + 1;

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
        
        snprintf(title, sizeof(title), logged_in ? "Usuario: %s" : "Login SGICPM", usuario_actual);

        const char *opciones[6]; 
        int num_opciones;
        
        if (!logged_in) {
            opciones[0] = "Iniciar Sesion";
            opciones[1] = "Registrarse";
            opciones[2] = "Salir";
            num_opciones = 3;
        } else {
            opciones[0] = "Ver Inventario";           // Antes "Ver Catalogo de Productos"
            opciones[1] = "Alertas de Caducidad";     // Movido de posición
            opciones[2] = "Comprar Producto";         // Nueva lógica pendiente
            opciones[3] = "Perfil de Usuario";        
            opciones[4] = "Cerrar Sesion";
            opciones[5] = "Salir";
            num_opciones = 6;
        }

        // --- DIBUJADO DEL MENÚ PRINCIPAL ---
        while (1) {
            werase(win);
            draw_box(win, title);
            mvwprintw(win, 2, 2, "Seleccione una opcion:");
            
            for (int i = 0; i < num_opciones; i++) {
                if (i == opcion_seleccionada) wattron(win, A_REVERSE); 
                mvwprintw(win, 4 + i, 4, "%s", opciones[i]);
                if (i == opcion_seleccionada) wattroff(win, A_REVERSE); 
            }
            
            mvwprintw(win, height - 2, 3, "Usa flechas (Arriba/Abajo) y ENTER");
            wrefresh(win);
            
            ch = wgetch(win);
            if (ch == KEY_UP) opcion_seleccionada = (opcion_seleccionada - 1 + num_opciones) % num_opciones;
            else if (ch == KEY_DOWN) opcion_seleccionada = (opcion_seleccionada + 1) % num_opciones;
            else if (ch == '\n' || ch == KEY_ENTER) break; 
        }

        // --- LÓGICA DE USUARIO NO LOGUEADO ---
        if (!logged_in) {
            if (opcion_seleccionada == 2) break; // Opcion de salir

            werase(win);
            draw_box(win, title);
            
            // Textos limpios y alineados
            mvwprintw(win, 2, 2, "Usuario: ");
            mvwprintw(win, 4, 2, "Password: ");
            
            // Instrucción posicionada en la parte inferior
            mvwprintw(win, height - 2, 2, "[Deja vacio y da ENTER p/volver]");
            wrefresh(win);
            
            curs_set(1); 
            echo();
            // Captura de usuario en la misma línea
            mvwgetnstr(win, 2, 11, user, 49); 
            noecho();
            
            // --- BOTÓN DE ATRÁS (USUARIO) ---
            if (strlen(user) == 0) {
                curs_set(0);
                continue; 
            }
            
            // Entrada de password oculta
            int i = 0;
            wmove(win, 4, 12); // Captura de password en la misma línea
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

            // --- BOTÓN DE ATRÁS (PASSWORD) ---
            if (strlen(pass) == 0) {
                continue;
            }

            char hashed_pass[65];
            hash_password(pass, hashed_pass);
            snprintf(payload_buffer, sizeof(payload_buffer), "%s %s", user, hashed_pass);

            werase(win); 
            draw_box(win, title);
            
            if (opcion_seleccionada == 0) { // LOGIN
                if (enviar_peticion_servidor(OP_LOGIN, payload_buffer)) {
                    logged_in = 1;
                    strncpy(usuario_actual, user, sizeof(usuario_actual));
                    mvwprintw(win, 4, 2, "BIENVENIDO, %s!", user);
                } else {
                    mvwprintw(win, 4, 2, "Error: %s", shm_ptr->respuesta);
                }
            } else if (opcion_seleccionada == 1) { // REGISTER
                enviar_peticion_servidor(OP_REGISTER, payload_buffer);
                mvwprintw(win, 4, 2, "%s", shm_ptr->respuesta);
            }
            
            mvwprintw(win, 8, 2, "[Presiona cualquier tecla]");
            wrefresh(win);
            wgetch(win);

        } else {
            // --- LÓGICA DE USUARIO LOGUEADO ---
            if (opcion_seleccionada == 0) { // VER INVENTARIO
                werase(win);
                draw_box(win, "Cargando Inventario...");
                wrefresh(win);

                if (enviar_peticion_servidor(OP_GET_PRODUCTS, "")) {
                    char productos[20][100];
                    int num_productos = 0;
                    
                    char *token = strtok(shm_ptr->respuesta, "\n");
                    while (token != NULL && num_productos < 20) {
                        strncpy(productos[num_productos], token, 99);
                        productos[num_productos][99] = '\0';
                        num_productos++;
                        token = strtok(NULL, "\n");
                    }

                    if (num_productos == 0) {
                        mvwprintw(win, 4, 2, "El inventario esta vacio.");
                        mvwprintw(win, height - 2, 2, "[Presiona cualquier tecla]");
                        wrefresh(win);
                        wgetch(win);
                    } else {
                        int salir_inventario = 0;
                        int scroll_offset = 0; // Para navegar si hay muchos productos

                        while (!salir_inventario) {
                            werase(win);
                            draw_box(win, "Inventario Actual ('q': Volver)");
                            
                            mvwprintw(win, 2, 4, "ID, Nombre, Cantidad, Caducidad");
                            mvwhline(win, 3, 2, ACS_HLINE, width - 4); 

                            // Dibujar la lista de productos (Solo lectura)
                            for (int i = 0; i < num_productos && i < height - 6; i++) {
                                mvwprintw(win, 4 + i, 4, "%s", productos[i + scroll_offset]);
                            }
                            
                            wrefresh(win);
                            
                            int input_ch = wgetch(win);
                            if (input_ch == KEY_UP && scroll_offset > 0) {
                                scroll_offset--;
                            } else if (input_ch == KEY_DOWN && scroll_offset < num_productos - (height - 6)) {
                                scroll_offset++;
                            } else if (input_ch == 'q' || input_ch == 'Q' || input_ch == '\n' || input_ch == KEY_ENTER) {
                                salir_inventario = 1;
                            }
                        }
                    }
                } else {
                    mvwprintw(win, 4, 2, "Error al cargar inventario.");
                    wrefresh(win);
                    wgetch(win);
                }
            }
            //--------------------------------------------------------------------------------------------------------------------------------------------------
            else if (opcion_seleccionada == 1) { // ALERTAS DE CADUCIDAD
                werase(win);
                draw_box(win, "Control Predictivo de Mermas");
                wrefresh(win);

                if (enviar_peticion_servidor(OP_CHECK_ALERTS, "")) {
                    int y = 2;
                    char *token = strtok(shm_ptr->respuesta, "\n");
                    
                    while (token != NULL && y < height - 3) {
                        mvwprintw(win, y++, 2, "%s", token);
                        token = strtok(NULL, "\n");
                    }
                } else {
                    mvwprintw(win, 2, 2, "Error: %s", shm_ptr->respuesta);
                }

                mvwprintw(win, height - 2, 2, "[Presiona cualquier tecla para volver]");
                wrefresh(win);
                wgetch(win);
            }
            //----------------------------------------------------------------------------------------------------------------------------------------------
            else if (opcion_seleccionada == 2) { // COMPRAR PRODUCTO
                int salir_compras = 0;
                int opcion_compra = 0;
                const char *opciones_compra[3] = {
                    "1. Ver Catalogo (Agregar a carrito)", 
                    "2. Ver Carrito (Comprar)", 
                    "3. Volver al Menu Principal"
                };

                // --- BUCLE DEL SUBMENÚ DE COMPRAS ---
                while (!salir_compras) {
                    werase(win);
                    draw_box(win, "Modulo de Compras");
                    mvwprintw(win, 2, 2, "Seleccione una opcion:");
                    
                    for (int i = 0; i < 3; i++) {
                        if (i == opcion_compra) wattron(win, A_REVERSE);
                        mvwprintw(win, 4 + i, 4, "%s", opciones_compra[i]);
                        if (i == opcion_compra) wattroff(win, A_REVERSE);
                    }
                    
                    mvwprintw(win, height - 2, 3, "Usa flechas y ENTER");
                    wrefresh(win);
                    
                    int ch_comp = wgetch(win);
                    if (ch_comp == KEY_UP) opcion_compra = (opcion_compra - 1 + 3) % 3;
                    else if (ch_comp == KEY_DOWN) opcion_compra = (opcion_compra + 1) % 3;
                    else if (ch_comp == '\n' || ch_comp == KEY_ENTER) {
                        
                        if (opcion_compra == 0) { // 1. VER CATALOGO Y AGREGAR
                            werase(win);
                            draw_box(win, "Cargando Catalogo...");
                            wrefresh(win);

                            if (enviar_peticion_servidor(OP_GET_CATALOG, "")) {
                                char productos[20][100];
                                int num_productos = 0;
                                char *token = strtok(shm_ptr->respuesta, "\n");
                                while (token != NULL && num_productos < 20) {
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
                                    int salir_catalogo = 0;
                                    while (!salir_catalogo) {
                                        werase(win);
                                        draw_box(win, "Catalogo (ENTER: Agregar | 'q': Volver)");
                                        mvwprintw(win, 2, 4, "ID, Nombre, Cantidad, Caducidad");
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
                                        else if (input_ch == 'q' || input_ch == 'Q') salir_catalogo = 1;
                                        else if (input_ch == '\n' || input_ch == KEY_ENTER) {
                                            // Extraer los datos del catalogo de proveedores
                                            char id_prod[20], nom_prod[100], cad_prod[20];
                                            
                                            // Leemos el formato del catalogo: ID, Nombre, Caducidad
                                            sscanf(productos[prod_seleccionado], " %[^,] , %[^,] , %s", id_prod, nom_prod, cad_prod);

                                            // Ventana emergente para preguntar cantidad a pedir
                                            werase(win);
                                            draw_box(win, "Pedido a Proveedor");
                                            mvwprintw(win, 2, 2, "Articulo: %s", nom_prod);
                                            mvwprintw(win, 4, 2, "Cantidad a pedir: ");
                                            wrefresh(win);
                                            
                                            char qty_str[10];
                                            echo();
                                            curs_set(1);
                                            mvwgetnstr(win, 4, 20, qty_str, 9);
                                            noecho();
                                            curs_set(0);
                                            
                                            int cantidad_pedida = atoi(qty_str);
                                            
                                            if (cantidad_pedida > 0) {
                                                char payload_carrito[1024];
                                                // Empaquetamos para el carrito: ID, Nombre, Cantidad pedida, Caducidad
                                                snprintf(payload_carrito, sizeof(payload_carrito), "%s|%s, %s, %d, %s", 
                                                         usuario_actual, id_prod, nom_prod, cantidad_pedida, cad_prod);
                                                
                                                enviar_peticion_servidor(OP_ADD_CART, payload_carrito);
                                                
                                                werase(win);
                                                draw_box(win, "Exito");
                                                mvwprintw(win, height / 2, 2, "Producto agregado a tu orden.");
                                            } else {
                                                werase(win);
                                                draw_box(win, "Error");
                                                mvwprintw(win, height / 2, 2, "Cantidad invalida.");
                                            }
                                            
                                            mvwprintw(win, height - 2, 2, "[Presiona cualquier tecla]");
                                            wrefresh(win);
                                            wgetch(win);
                                        }
                                    }
                                }
                            }
                        }
                        else if (opcion_compra == 1) { // 2. VER CARRITO Y PAGAR
                            werase(win);
                            draw_box(win, "Mi Carrito");
                            
                            if (enviar_peticion_servidor(OP_GET_CART, usuario_actual)) {
                                mvwprintw(win, 2, 2, "Articulos guardados:");
                                
                                int y = 4;
                                char *token = strtok(shm_ptr->respuesta, "\n");
                                while (token != NULL && y < height - 3) {
                                    mvwprintw(win, y++, 2, "%s", token);
                                    token = strtok(NULL, "\n");
                                }
                                
                                mvwprintw(win, height - 2, 2, "[ENTER] Comprar | [q] Volver");
                                wrefresh(win);
                                
                                int input_cart = wgetch(win);
                                if (input_cart == '\n' || input_cart == KEY_ENTER) {
                                    werase(win);
                                    draw_box(win, "Procesando transaccion...");
                                    wrefresh(win);
                                    
                                    // Mandamos la orden de compra al servidor
                                    enviar_peticion_servidor(OP_BUY_CART, usuario_actual);
                                    
                                    werase(win);
                                    draw_box(win, "Estatus de Compra");
                                    mvwprintw(win, 4, 2, "%s", shm_ptr->respuesta);
                                    mvwprintw(win, height - 2, 2, "[Presiona cualquier tecla]");
                                    wrefresh(win);
                                    wgetch(win);
                                } 
                                // Si presiona 'q' o cualquier otra tecla que no sea ENTER, 
                                // el if no hace nada y simplemente regresa al submenú.
                            } else {
                                mvwprintw(win, 4, 2, "Error al enlazar con el carrito.");
                                wgetch(win);
                            }
                        }
                        else if (opcion_compra == 2) { // 3. SALIR
                            salir_compras = 1; // Rompe el bucle del submenú
                        }
                    }
                }
            }
            else if (opcion_seleccionada == 3) { // MODIFICAR PERFIL
                char nuevo_user[50], nuevo_pass[50];
                werase(win);
                draw_box(win, "Configuracion de Perfil");
                mvwprintw(win, 2, 2, "Nuevo Usuario: ");
                mvwprintw(win, 4, 2, "Nuevo Password: ");
                
                // --- AVISO VISUAL PARA EL USUARIO ---
                mvwprintw(win, height - 2, 2, "[Deja vacio y da ENTER p/volver]");
                wrefresh(win);
                
                curs_set(1); 
                echo();
                mvwgetnstr(win, 2, 17, nuevo_user, 49);
                noecho();

                // --- LÓGICA DE SALIDA (Atrás) ---
                if (strlen(nuevo_user) == 0) {
                    curs_set(0);
                    continue; // Regresa al menú principal
                }
                
                // Captura de password oculta con asteriscos
                int i = 0;
                wmove(win, 4, 18);
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
                        nuevo_pass[i++] = ch;
                        waddch(win, '*'); 
                    }
                }
                nuevo_pass[i] = '\0';
                curs_set(0); 

                // --- LÓGICA DE SALIDA (Atrás) EN PASSWORD ---
                if (strlen(nuevo_pass) == 0) {
                    continue; // Regresa al menú principal
                }

                if (strlen(nuevo_user) > 0 && strlen(nuevo_pass) > 0) {
                    char nuevo_hash[65];
                    hash_password(nuevo_pass, nuevo_hash);
                    
                    // Empaquetar datos: usuario_actual|nuevo_usuario|nuevo_hash
                    snprintf(payload_buffer, sizeof(payload_buffer), "%s|%s|%s", usuario_actual, nuevo_user, nuevo_hash);
                    
                    // Limpiamos la pantalla antes de mostrar el resultado
                    werase(win);
                    draw_box(win, "Configuracion de Perfil");

                    if (enviar_peticion_servidor(OP_UPDATE_PROFILE, payload_buffer)) {
                        // Cambiar localmente el nombre del usuario para reflejar el cambio en la barra de titulo
                        strncpy(usuario_actual, nuevo_user, sizeof(usuario_actual));
                        mvwprintw(win, 4, 2, "%s", shm_ptr->respuesta);
                    } else {
                        mvwprintw(win, 4, 2, "Error: %s", shm_ptr->respuesta);
                    }
                } else {
                    werase(win);
                    draw_box(win, "Configuracion de Perfil");
                    mvwprintw(win, 4, 2, "Campos vacios. Operacion cancelada.");
                }
                
                mvwprintw(win, height - 2, 2, "[Presiona cualquier tecla para volver]");
                wrefresh(win);
                wgetch(win);
            }
            else if (opcion_seleccionada == 4) { // Cerrar Sesión
                logged_in = 0;
                usuario_actual[0] = '\0'; 
                werase(win);
                draw_box(win, "Login System");
                mvwprintw(win, 4, 2, "Sesion cerrada con exito.");
                mvwprintw(win, 8, 2, "[Presiona cualquier tecla]");
                wrefresh(win);
                wgetch(win);
            }
            else if (opcion_seleccionada == 5) { // Salir
                break; 
            }
        }
    }

    // ==== FASE 3: LIMPIEZA Y CIERRE ====
    enviar_peticion_servidor(OP_EXIT, "");
    delwin(win);
    endwin();
    shmdt(shm_ptr);
    shmctl(shmid, IPC_RMID, NULL);
    return 0;
}
