#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <signal.h>
#include <pthread.h>

#define BACKLOG 5

int running = 1;

// the argument we will pass to the connection-handler threads
struct connection
{
    struct sockaddr_storage addr;
    socklen_t addr_len;
    int fd;
    struct tree *binaryTree;
};
struct tree
{
    struct node *head;
    pthread_mutex_t lock;
    int size;
};
struct node
{
    char *key;
    char *value;
    struct node *leftChild;
    struct node *rightChild;
};
void free_tree(struct node *root);
void inorder(struct node *root);
struct node *insert(struct node *root, char *key, char *value);
struct node *new_node(char *key, char *value);
int server(char *port);
void *communication(void *arg);
int init(struct tree *binaryTree);
int destroyConnection(struct connection *c, FILE *fp, FILE *fout);
char *getKey(struct node *root, char *key);
struct node *findMin(struct node *root);
struct node *deleteKey(struct tree *tree, char *key);
int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("Usage: %s [port]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    (void)server(argv[1]);
    return EXIT_SUCCESS;
}

void handler(int signal)
{
    running = 0;
}

int server(char *port)
{
    struct addrinfo hint, *info_list, *info;
    struct connection *con;
    int error, sfd;
    pthread_t tid;

    // initialize hints
    memset(&hint, 0, sizeof(struct addrinfo));
    hint.ai_family = AF_UNSPEC;
    hint.ai_socktype = SOCK_STREAM;
    hint.ai_flags = AI_PASSIVE;
    // setting AI_PASSIVE means that we want to create a listening socket

    // get socket and address info for listening port
    // - for a listening socket, give NULL as the host name (because the socket is on
    //   the local host)
    error = getaddrinfo(NULL, port, &hint, &info_list);
    if (error != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));

        return -1;
    }

    // attempt to create socket
    for (info = info_list; info != NULL; info = info->ai_next)
    {
        sfd = socket(info->ai_family, info->ai_socktype, info->ai_protocol);

        // if we couldn't create the socket, try the next method
        if (sfd == -1)
        {
            continue;
        }

        // if we were able to create the socket, try to set it up for
        // incoming connections;
        //
        // note that this requires two steps:
        // - bind associates the socket with the specified port on the local host
        // - listen sets up a queue for incoming connections and allows us to use accept
        if ((bind(sfd, info->ai_addr, info->ai_addrlen) == 0) &&
            (listen(sfd, BACKLOG) == 0))
        {
            break;
        }

        // unable to set it up, so try the next method
        close(sfd);
    }

    if (info == NULL)
    {
        // we reached the end of result without successfuly binding a socket
        fprintf(stderr, "Could not bind\n");
        exit(EXIT_FAILURE);
        return -1;
    }

    freeaddrinfo(info_list);

    struct sigaction act;
    act.sa_handler = handler;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    sigaction(SIGINT, &act, NULL);

    sigset_t mask;

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    struct tree *binaryTree = malloc(sizeof(struct tree));
    if (binaryTree == NULL)
    {
    }
    init(binaryTree);
    // at this point sfd is bound and listening
    printf("Waiting for connection\n");
    while (running)
    {
        // create argument struct for child thread
        con = malloc(sizeof(struct connection));
        con->addr_len = sizeof(struct sockaddr_storage);
        // addr_len is a read/write parameter to accept
        // we set the initial value, saying how much space is available
        // after the call to accept, this field will contain the actual address length
        con->binaryTree = binaryTree;
        // wait for an incoming connection
        con->fd = accept(sfd, (struct sockaddr *)&con->addr, &con->addr_len);
        // we provide
        // sfd - the listening socket
        // &con->addr - a location to write the address of the remote host
        // &con->addr_len - a location to write the length of the address
        //
        // accept will block until a remote host tries to connect
        // it returns a new socket that can be used to communicate with the remote
        // host, and writes the address of the remote hist into the provided location

        // if we got back -1, it means something went wrong
        if (con->fd == -1)
        {

            perror("accept");
            close(con->fd);
            free(con);
            continue;
        }

        // temporarily block SIGINT (child will inherit mask)
        error = pthread_sigmask(SIG_BLOCK, &mask, NULL);
        if (error != 0)
        {
            fprintf(stderr, "sigmask: %s\n", strerror(error));
            abort();
        }

        // spin off a worker thread to handle the remote connection
        error = pthread_create(&tid, NULL, communication, con);

        // if we couldn't spin off the thread, clean up and wait for another connection
        if (error != 0)
        {
            fprintf(stderr, "Unable to create thread: %d\n", error);

            continue;
        }

        // otherwise, detach the thread and wait for the next connection request
        pthread_detach(tid);

        // unblock SIGINT
        error = pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
        if (error != 0)
        {
            fprintf(stderr, "sigmask: %s\n", strerror(error));
            abort();
        }
    }

    puts("No longer listening.");
    free_tree(binaryTree->head);
    free(binaryTree);
    pthread_mutex_destroy(&binaryTree->lock);
    pthread_detach(pthread_self());
    pthread_exit(NULL);

    // never reach here
    return 0;
}

#define BUFSIZE 8

void *communication(void *arg)
{
    char host[100], port[10];
    struct connection *c = (struct connection *)arg;
    int error;
    struct tree *binaryTree = c->binaryTree;
    // find out the name and port of the remote host
    error = getnameinfo((struct sockaddr *)&c->addr, c->addr_len, host, 100, port, 10, NI_NUMERICSERV);
    // we provide:
    // the address and its length
    // a buffer to write the host name, and its length
    // a buffer to write the port (as a string), and its length
    // flags, in this case saying that we want the port as a number, not a service name
    if (error != 0)
    {
        fprintf(stderr, "getnameinfo: %s", gai_strerror(error));
        close(c->fd);
        return NULL;
    }

    printf("[%s:%s] connection\n", host, port);
    int sock2 = dup(c->fd);

    FILE *fp = fdopen(c->fd, "r");
    if (sock2 == -1)
    {
        fprintf(fp, "ERR\nSERV\n");
        fflush(fp);
    }
    FILE *fout = fdopen(sock2, "w");
    if (fp == NULL || fout == NULL)
    {
        fprintf(fp, "ERR\nSERV\n");
        fflush(fp);
    }
    int bytesRemaining;
    while (1)
    {
        char option[sizeof(int) * 4];
        //char *option = malloc(sizeof(int) * 4);
        int cha = getc(fp);
        int k = 0;
        while ((cha != EOF) && cha != '\n' && k < 3) //only getting three characters if option
        {
            option[k] = cha;
            cha = getc(fp);
            k++;
        }
        if (cha != '\n' || k != 3)
        {
            fprintf(fout, "ERR\nBAD\n");
            fflush(fout);
            pthread_mutex_unlock(&binaryTree->lock);
            destroyConnection(c, fp, fout);
            return NULL;
        }
        option[k] = '\0';

        if (!strcmp(option, "GET"))
        {
            if (fscanf(fp, "%d\n", &bytesRemaining) != EOF)
                ;
            if (bytesRemaining == 0)
            {
                fprintf(fout, "ERR\nBAD\n");
                fflush(fout);
                destroyConnection(c, fp, fout);
                return NULL;
            }
            char key[bytesRemaining];
            int ch;
            int i = 0;
            while (1)
            {
                ch = getc(fp);
                if (ch == EOF)
                    break;
                if (ch == '\n')
                    break;
                key[i] = ch;
                i++;
            }
            key[i] = '\0';

            if (strlen(key) + 1 != bytesRemaining)
            {
                fprintf(fout, "ERR\nLEN\n");
                fflush(fout);
                destroyConnection(c, fp, fout);
                return NULL;
            }
            bytesRemaining = 0;
            pthread_mutex_lock(&binaryTree->lock);
            char *value = getKey(binaryTree->head, key);
            pthread_mutex_unlock(&binaryTree->lock);
            if (value == NULL)
            {
                fprintf(fout, "KNF\n");
                fflush(fout);
            }
            else
            {
                fprintf(fout, "OKG\n%ld\n%s\n", strlen(value) + 1, value);
                fflush(fout);
            }
        }
        else if (!strcmp(option, "SET"))
        {
            if (fscanf(fp, "%d\n", &bytesRemaining) != EOF)
                ;
            if (bytesRemaining == 0)
            {
                fprintf(fout, "ERR\nBAD\n");
                fflush(fout);
                destroyConnection(c, fp, fout);
                return NULL;
            }
            char key[bytesRemaining];
            int ch;
            int i = 0;
            while (1)
            {
                ch = getc(fp);
                if (ch == EOF)
                {
                    break;
                }
                if (ch == '\n')
                    break;
                key[i] = ch;
                i++;
            }
            key[i] = '\0';

            char payload[bytesRemaining];
            int j = 0;
            while (1)
            {
                ch = getc(fp);
                if (ch == EOF)
                    break;
                if (ch == '\n')
                    break;
                payload[j] = ch;
                j++;
            }
            payload[j] = '\0';
            if (strlen(key) + strlen(payload) + 2 != bytesRemaining)
            {
                fprintf(fout, "ERR\nLEN\n");
                fflush(fout);
                destroyConnection(c, fp, fout);
                return NULL;
            }
            pthread_mutex_lock(&binaryTree->lock);
            bytesRemaining = 0;
            binaryTree->head = insert(binaryTree->head, key, payload);
            fprintf(fout, "OKS\n");
            fflush(fout);
            pthread_mutex_unlock(&binaryTree->lock);
            //printf("%s\n", binaryTree->head->key);
            //free(option);
        }
        else if (!strcmp(option, "DEL"))
        {
            if (fscanf(fp, "%d\n", &bytesRemaining) != EOF)
                ;
            if (bytesRemaining == 0)
            {
                fprintf(fout, "ERR\nBAD\n");
                fflush(fout);
                destroyConnection(c, fp, fout);
                return NULL;
            }
            char key[bytesRemaining];
            int ch;
            int i = 0;
            while (1)
            {
                ch = getc(fp);
                if (ch == EOF)
                    break;
                if (ch == '\n')
                    break;
                key[i] = ch;
                i++;
            }
            key[i] = '\0';
            if (strlen(key) + 1 != bytesRemaining)
            {
                fprintf(fout, "ERR\nLEN\n");
                fflush(fout);
                destroyConnection(c, fp, fout);
                return NULL;
            }
            bytesRemaining = 0;
            pthread_mutex_lock(&binaryTree->lock);
            struct node *value = NULL;
            if (binaryTree->head != NULL)
            {

                value = deleteKey(binaryTree, key);
            }
            pthread_mutex_unlock(&binaryTree->lock);
            if (value == NULL)
            {
                fprintf(fout, "KNF\n");
                fflush(fout);
            }
            else
            {
                fprintf(fout, "OKD\n%ld\n%s\n", strlen(value->value) + 1, value->value);
                fflush(fout);
                if (value == binaryTree->head)
                    binaryTree->head = NULL;
                free(value->value);
                free(value->key);
                free(value);
                value = NULL;
                inorder(binaryTree->head);
            }
        }
        else
        {
            fprintf(fout, "ERR\nBAD\n");
            fflush(fout);
            //pthread_mutex_unlock(&binaryTree->lock);
            destroyConnection(c, fp, fout);
            return NULL;
        }
        //free(option);
    }
    destroyConnection(c, fp, fout);
    return NULL;
}
struct node *insert(struct node *root, char *key, char *value)
{
    //root is null means tree is empty new node is now root
    if (root == NULL)
    {
        return new_node(key, value);
    }
    //MEANS THEY ARE EQUAL KEY MUST SET THE KEY TO NEW VALUE
    else if (!strcmp(root->key, key))
    {
        free(root->value);
        root->value = malloc(strlen(value) + 1);
        strcpy(root->value, value);
        return root;
    }
    else if (strcmp(root->key, key) < 0)
        root->rightChild = insert(root->rightChild, key, value);
    else if (strcmp(root->key, key) > 0)
        root->leftChild = insert(root->leftChild, key, value);
    return root;
}
struct node *new_node(char *key, char *value)
{
    //sets the node
    struct node *p = malloc(sizeof(struct node));
    p->key = malloc(strlen(key) + 1);
    p->value = malloc(strlen(value) + 1);
    strcpy(p->key, key);
    strcpy(p->value, value);
    p->leftChild = NULL;
    p->rightChild = NULL;

    return p;
}
void inorder(struct node *root)
{

    if (root != NULL) // checking if the root is not null
    {
        inorder(root->leftChild);
        printf("key: %s  value %s \n", root->key, root->value);
        inorder(root->rightChild);
    }
}
void free_tree(struct node *root)
{
    if (root != NULL)
    {
        free_tree(root->rightChild);
        free(root->key); //if data was heap allocated, need to free it
        free(root->value);
        free_tree(root->leftChild);
        free(root);
    }
}
int init(struct tree *binaryTree)
{
    binaryTree->head = NULL;
    pthread_mutex_init(&binaryTree->lock, NULL);
    return 0;
}
int destroyConnection(struct connection *c, FILE *fp, FILE *fout)
{
    close(c->fd);
    free(c);
    fclose(fp);
    fclose(fout);
    return 0;
}
char *getKey(struct node *root, char *key)
{
    //root is null means tree is empty new node is now root
    char *value = NULL;
    if (root == NULL)
    {
        return NULL;
    }
    //MEANS THEY ARE EQUAL KEY MUST SET THE KEY TO NEW VALUE
    else if (!strcmp(root->key, key))
    {
        return root->value;
    }
    else if (strcmp(root->key, key) < 0)
        value = getKey(root->rightChild, key);
    else if (strcmp(root->key, key) > 0)
        value = getKey(root->leftChild, key);
    return value;
}
struct node *deleteKey(struct tree *tree, char *key)
{
    struct node *root = tree->head;

    //root is null means tree is empty new node is now root
    struct node *parent = root;
    struct node *temp = root;
    int side = 0;
    while (temp != NULL)
    {

        //found the key now find the minimum and adjust the parent
        if (!strcmp(temp->key, key))
        {

            //printf("PARENT OF KEY %s\n", parent->key);
            //printf("FOUND KEY %s\n", temp->key);
            struct node *replacement = findMin(temp);
            if (replacement == NULL && parent != temp)
            { //FREE THE ROOT AND RETURN ITS AN EMPTY TREE
                if (!side)
                    parent->rightChild = NULL;
                else
                    parent->leftChild = NULL;
                return temp;
            }
            else if (replacement == NULL)
            {
                return temp;
            }
            if (temp == root)
            {

                struct node *temp = *(&root);
                replacement->rightChild = root->rightChild;
                replacement->leftChild = root->leftChild;
                root = replacement;
                tree->head = root;
                //printf("ROOT AFTER DELTEe %s\n", root->key);
                return temp;
            }
            else
            {
                if (!side)
                    parent->rightChild = replacement;
                else
                    parent->leftChild = replacement;
                replacement->rightChild = temp->rightChild;
                replacement->leftChild = temp->leftChild;
                return temp;
            }
            break;
        }
        else if (strcmp(temp->key, key) < 0)
        {
            side = 0;
            parent = temp;
            temp = temp->rightChild;
        }
        else if (strcmp(temp->key, key) > 0)
        {
            side = 1;
            parent = temp;
            temp = temp->leftChild;
        }
    }

    return NULL;
}
struct node *findMin(struct node *root)
{
    struct node *parent;
    int check = 0;
    if (root->leftChild == NULL && root->rightChild == NULL)
        return NULL;
    if (root->leftChild == NULL)
    {
        //then we return root->right
        parent = root;
        struct node *temp = root->rightChild;
        parent->rightChild = temp->rightChild;
        parent->leftChild = temp->leftChild;
        return temp;
    }
    else
    {
        parent = root;
        root = root->leftChild;
        while (root->rightChild != NULL)
        {
            check = 1;
            parent = root;
            root = root->rightChild;
        }
        struct node *temp;
        if (check)
        {
            temp = parent->rightChild;
            parent->rightChild = temp->leftChild;
        }
        else
        {
            temp = parent->leftChild;
            parent->leftChild = temp->leftChild;
        }
        return temp;
    }
    return NULL;
}