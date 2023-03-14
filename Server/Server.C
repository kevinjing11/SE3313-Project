#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

#define PORT 8080
#define BUFFER_SIZE 256
#define BOARD_SIZE 3
#define MAX_PLAYERS 2


int game_instance_counter = 0;
typedef struct
{
    int client_sock;
    int player_id;
    int game_id;
} player_data_t;

typedef enum
{
    ONGOING,
    PLAYER1_WINS,
    PLAYER2_WINS,
    DRAW
} game_status_t;

// Game Instance definition
typedef struct game_instance
{
    int game_id;
    int id;
    char board[BOARD_SIZE][BOARD_SIZE];
    int current_player;
    player_data_t players[MAX_PLAYERS];
    int num_players;
    bool game_over;
    pthread_t thread;
    struct game_instance* next;
} game_instance_t;

// Global variables
game_instance_t* game_instance_list = NULL;
sem_t games_list_mutex;
int game_id_counter = 0;
bool server_running = true;

// Function prototypes
void *handle_game_instance(void *game_data);
void *handle_client(void *data);
void handle_sigint(int sig);
game_instance_t *create_game_instance();
game_instance_t *find_game_instance(int game_id);
void remove_game_instance(int game_id);
void cleanup_game_instance(game_instance_t *game);

sem_t semaphore;

char board[BOARD_SIZE][BOARD_SIZE];
int current_player;

// Utility Functions
void send_board(int client_sock, char board[3][3])
{
    char msg[BUFFER_SIZE];
    for (int i = 0; i < 3; i++)
    {
        snprintf(msg, sizeof(msg), "%c | %c | %c\n", board[i][0], board[i][1], board[i][2]);
        send(client_sock, msg, strlen(msg), 0);
        if (i < 2)
        {
            send(client_sock, "---------\n", 10, 0);
        }
    }
}

bool parse_move(char *move, int *row, int *col)
{
    int move_int = atoi(move);
    if (move_int >= 1 && move_int <= 9)
    {
        *row = (move_int - 1) / 3;
        *col = (move_int - 1) % 3;
        return true;
    }
    return false;
}

bool is_valid_move(char board[3][3], int row, int col)
{
    // Check if the selected cell is not already occupied by a player's symbol
    return board[row][col] != 'X' && board[row][col] != 'O';
}

// Update current status of the game, return to client
game_status_t check_game_status(char board[3][3])
{
    // Check rows, columns, and diagonals for a win
    for (int i = 0; i < 3; i++)
    {
        if ((board[i][0] == board[i][1] && board[i][1] == board[i][2]) ||
            (board[0][i] == board[1][i] && board[1][i] == board[2][i]))
        {
            return board[i][i] == 'X' ? PLAYER1_WINS : PLAYER2_WINS;
        }
    }

    if ((board[0][0] == board[1][1] && board[1][1] == board[2][2]) ||
        (board[0][2] == board[1][1] && board[1][1] == board[2][0]))
    {
        return board[1][1] == 'X' ? PLAYER1_WINS : PLAYER2_WINS;
    }

    // Check for a draw
    bool draw = true;
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            if (board[i][j] != 'X' && board[i][j] != 'O')
            {
                draw = false;
                break;
            }
        }
        if (!draw)
            break;
    }

    return draw ? DRAW : ONGOING;
}

// Game instance management functions:
game_instance_t *create_game_instance(int player1_sock)
{
   game_instance_t *new_game_instance = (game_instance_t *)calloc(1, sizeof(game_instance_t));
    new_game_instance->id = game_instance_counter++;
    new_game_instance->num_players = 0;
    new_game_instance->game_over = false;
    memset(new_game_instance->board, ' ', sizeof(new_game_instance->board));
    new_game_instance->current_player = 0;
    new_game_instance->next = game_instance_list;
    game_instance_list = new_game_instance;

    //Initialize game board:
    for (int i = 0; i < BOARD_SIZE; i++) {
    for (int j = 0; j < BOARD_SIZE; j++) {
        new_game_instance->board[i][j] = '1' + (i * BOARD_SIZE) + j;
    }
}

    return new_game_instance;
}

game_instance_t *find_game_instance(int game_id)
{
    game_instance_t *current = game_instance_list;
    while (current)
    {
        if (current->id == game_id)
        {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

void remove_game_instance(int game_id)
{
    game_instance_t *current = game_instance_list;
    game_instance_t *prev = NULL;
    while (current)
    {
        if (current->id == game_id)
        {
            if (prev)
            {
                prev->next = current->next;
            }
            else
            {
                game_instance_list = current->next;
            }
            free(current);
            break;
        }
        prev = current;
        current = current->next;
    }
}

// Client Handler
void *handle_client(void *data)
{
    player_data_t *player_data = (player_data_t *)data;
    int client_sock = player_data->client_sock;

    // Welcome message and prompt user to join or create a game
    char msg[BUFFER_SIZE];
    snprintf(msg, sizeof(msg), "Welcome to Tic-Tac-Toe! Enter 'join <game_id>' to join a game or 'create' to create a new game:\n");
    send(client_sock, msg, strlen(msg), 0);

    while (true)
    {
        // Receive user input
        char user_input[BUFFER_SIZE];
        memset(user_input, 0, sizeof(user_input));
        recv(client_sock, user_input, sizeof(user_input) - 1, 0);
        char command[BUFFER_SIZE], game_id_str[BUFFER_SIZE];
        sscanf(user_input, "%s %s", command, game_id_str);

        if (strcmp(command, "join") == 0)
        {
            // Find game instance by ID and add player
            int game_id = atoi(game_id_str);
            sem_wait(&games_list_mutex);
          game_instance_t *game = find_game_instance(game_id);
            if (game != NULL && game->num_players < MAX_PLAYERS)
            {
                player_data->game_id = game->game_id;
                player_data->player_id = game->num_players;
                game->players[game->num_players++] = *player_data;
            }
            else
            {
                snprintf(msg, sizeof(msg), "Failed to join game. Enter 'join <game_id>' to join a game or 'create' to create a new game:\n");
                send(client_sock, msg, strlen(msg), 0);
            }
            sem_post(&games_list_mutex);
        }
        else if (strcmp(command, "create") == 0)
        {
            // Create a new game instance and add player
            sem_wait(&games_list_mutex);
            game_instance_t *game = create_game_instance();
            player_data->game_id = game->id;
            player_data->player_id = game->num_players;
            game->players[game->num_players++] = *player_data;
            sem_post(&games_list_mutex);
        }
        else
        {
            // Invalid input, prompt user again
            snprintf(msg, sizeof(msg), "Invalid input. Enter 'join <game_id>' to join a game or 'create' to create a new game:\n");
            send(client_sock, msg, strlen(msg), 0);
            continue;
        }

        // If successfully joined or created a game, break the loop
        if (player_data->game_id >= 0)
            break;

        if (player_data->game_id >= 0)
        {
            game_instance_t *game = find_game_instance(player_data->game_id);
            if (game->num_players == MAX_PLAYERS)
            {
               pthread_create(&game->thread, NULL, handle_game_instance, game);
            }
        }
    }

    // Wait for the game instance to finish
    game_instance_t *game = find_game_instance(player_data->game_id);
    if (game != NULL)
    {
        pthread_join(game->thread, NULL);
    }

    // Clean up resources and exit
    close(client_sock);
    free(player_data);
    return NULL;
}

// Handle comms w clients and game state management
void *handle_game_instance(void *game_data)
{
    game_instance_t *game = (game_instance_t *)game_data;

    // Initialize the game state
    char board[3][3] = {
        {'1', '2', '3'},
        {'4', '5', '6'},
        {'7', '8', '9'}};
    int current_player = 0;

    // Send initial board state to the players
    for (int i = 0; i < game->num_players; i++)
    {
        send(game->players[i].client_sock, "Game has started!\n", 17, 0);
        send_board(game->players[i].client_sock, board);
    }

    // Main game loop
    while (!game->game_over)
    {
        // Send a message to the current player requesting their move
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "Your move (player %d):\n", current_player + 1);
        send(game->players[current_player].client_sock, msg, strlen(msg), 0);

        // Receive the move from the current player
        char move[BUFFER_SIZE];
        memset(move, 0, sizeof(move));
        recv(game->players[current_player].client_sock, move, sizeof(move) - 1, 0);

        // Update the board with the player's move and check the game status
        int row, col;
        if (parse_move(move, &row, &col))
        {
            if (is_valid_move(board, row, col))
            {
                board[row][col] = current_player == 0 ? 'X' : 'O';
                game_status_t status = check_game_status(board);

                // Send the updated board state to all players
                for (int i = 0; i < game->num_players; i++)
                {
                    send_board(game->players[i].client_sock, board);
                }

                // Handle game over conditions
                switch (status)
                {
                case PLAYER1_WINS:
                    game->game_over = true;
                    send(game->players[0].client_sock, "You won!\n", 8, 0);
                    send(game->players[1].client_sock, "You lost!\n", 9, 0);
                    break;
                case PLAYER2_WINS:
                    game->game_over = true;
                    send(game->players[0].client_sock, "You lost!\n", 9, 0);
                    send(game->players[1].client_sock, "You won!\n", 8, 0);
                    break;
                case DRAW:
                    game->game_over = true;
                    for (int i = 0; i < game->num_players; i++)
                    {
                        send(game->players[i].client_sock, "It's a draw!\n", 12, 0);
                    }
                    break;
                case ONGOING:
                    current_player = (current_player + 1) % game->num_players;
                    break;
                }
            }
        }
    }

    // Clean up the game instance
    sem_wait(&games_list_mutex);
    remove_game_instance(game->id);
    sem_post(&games_list_mutex);
    return NULL;
}

// Logic Handling for the game
bool check_win()
{
    for (int i = 0; i < BOARD_SIZE; ++i)
    {
        if (board[i][0] == board[i][1] && board[i][1] == board[i][2] && board[i][0] != ' ')
            return true;
        if (board[0][i] == board[1][i] && board[1][i] == board[2][i] && board[0][i] != ' ')
            return true;
    }

    if (board[0][0] == board[1][1] && board[1][1] == board[2][2] && board[0][0] != ' ')
        return true;
    if (board[0][2] == board[1][1] && board[1][1] == board[2][0] && board[0][2] != ' ')
        return true;

    return false;
}

bool check_draw()
{
    for (int i = 0; i < BOARD_SIZE; ++i)
    {
        for (int j = 0; j < BOARD_SIZE; ++j)
        {
            if (board[i][j] == ' ')
                return false;
        }
    }
    return true;
}

void send_data(int client_sock, const char *data)
{
    send(client_sock, data, strlen(data), 0);
}

// Signal Handler for Termination
void handle_sigint(int sig)
{
    server_running = false;
    sem_wait(&games_list_mutex);
    game_instance_t *game = games_list;
    while (game != NULL)
    {
        game->game_over = true;
        game = game->next;
    }
    sem_post(&games_list_mutex);
}

int main()
{
    // Set Up socket and handler for input
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    pthread_t threads[MAX_PLAYERS];
    player_data_t players[MAX_PLAYERS];

    memset(board, ' ', sizeof(board));
    current_player = 0;
    sem_init(&semaphore, 0, 1);

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(server_sock, MAX_PLAYERS) < 0)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0)
        {
            perror("accept");
            exit(EXIT_FAILURE);
        }
        players[i].client_sock = client_sock;
        players[i].player_id = i;
        pthread_create(&threads[i], NULL, handle_client, &players[i]);
    }

    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        pthread_join(threads[i], NULL);
    }

    while (server_running)
    {
        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0)
        {
            perror("accept");
            continue;
        }
        pthread_t client_thread;
        player_data_t *player_data = (player_data_t *)malloc(sizeof(player_data_t));
        player_data->client_sock = client_sock;
        pthread_create(&client_thread, NULL, handle_client, player_data);
        pthread_detach(client_thread);
    }

    // Signal Handler
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    // For server close

    close(server_sock);
    sem_destroy(&semaphore);

    return 0;
}
