/* =====================================================================
   servidor.c — servidor HTTP simples em C puro (sem bibliotecas externas)
   para o backend do site "Memórias".

   Guarda fotos, textos e recados como arquivos dentro de data/.
   Não usa JSON de verdade: a comunicação com o navegador usa um formato
   de texto simples definido por nós mesmos (ver README.md), o que deixa
   o parser bem mais simples de escrever e entender em C.

   Compilar:   gcc -O2 -Wall -o servidor server.c
   Rodar:      ./servidor            (escuta na porta 8080)
===================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define PORTA 8080
#define BUFFER_INICIAL 8192

/* ---------------------------------------------------------------------
   utilidades gerais
--------------------------------------------------------------------- */

static void garantir_diretorio(const char *caminho){
    if(mkdir(caminho, 0755) != 0 && errno != EEXIST){
        fprintf(stderr, "aviso: não consegui criar %s (%s)\n", caminho, strerror(errno));
    }
}

/* login é só pra duas pessoas: valida contra uma lista fixa,
   também evita qualquer tentativa de path traversal no nome do usuário */
static int usuario_valido(const char *usuario){
    return strcmp(usuario, "amanda") == 0 || strcmp(usuario, "caetano") == 0;
}

static char *ler_arquivo_completo(const char *caminho, long *tamanho_saida){
    FILE *f = fopen(caminho, "rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END);
    long tam = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(tam + 1);
    if(!buf){ fclose(f); return NULL; }
    size_t lidos = fread(buf, 1, tam, f);
    buf[lidos] = '\0';
    fclose(f);
    if(tamanho_saida) *tamanho_saida = (long)lidos;
    return buf;
}

static int escrever_arquivo(const char *caminho, const char *conteudo, size_t tamanho){
    FILE *f = fopen(caminho, "wb");
    if(!f) return 0;
    fwrite(conteudo, 1, tamanho, f);
    fclose(f);
    return 1;
}

static char *gerar_id(void){
    static char id[32];
    snprintf(id, sizeof(id), "%lx%04x", (long)time(NULL), rand() % 65536);
    return id;
}

/* decodifica %XX e + de uma string vinda de encodeURIComponent */
static char *url_decode(const char *src){
    size_t len = strlen(src);
    char *out = malloc(len + 1);
    size_t j = 0;
    for(size_t i = 0; i < len; i++){
        if(src[i] == '%' && i + 2 < len && isxdigit((unsigned char)src[i+1]) && isxdigit((unsigned char)src[i+2])){
            char hex[3] = { src[i+1], src[i+2], 0 };
            out[j++] = (char) strtol(hex, NULL, 16);
            i += 2;
        } else if(src[i] == '+'){
            out[j++] = ' ';
        } else {
            out[j++] = src[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* escapa aspas e barras invertidas e quebras de linha para caber num campo de string JSON */
static char *escapar_json(const char *src){
    size_t len = strlen(src);
    char *out = malloc(len * 2 + 1); /* pior caso: todo caractere escapado */
    size_t j = 0;
    for(size_t i = 0; i < len; i++){
        unsigned char c = src[i];
        if(c == '"' || c == '\\'){ out[j++] = '\\'; out[j++] = c; }
        else if(c == '\n'){ out[j++] = '\\'; out[j++] = 'n'; }
        else if(c == '\r'){ /* ignora */ }
        else { out[j++] = c; }
    }
    out[j] = '\0';
    return out;
}

/* remove '\r' e '\n' finais de uma linha lida manualmente */
static void tirar_quebra(char *s){
    size_t l = strlen(s);
    while(l > 0 && (s[l-1] == '\n' || s[l-1] == '\r')){ s[--l] = '\0'; }
}

/* ---------------------------------------------------------------------
   buffer dinâmico para montar respostas
--------------------------------------------------------------------- */
typedef struct {
    char *dados;
    size_t tamanho;
    size_t capacidade;
} Buffer;

static void buf_iniciar(Buffer *b){
    b->capacidade = 4096;
    b->dados = malloc(b->capacidade);
    b->dados[0] = '\0';
    b->tamanho = 0;
}
static void buf_add(Buffer *b, const char *texto){
    size_t add = strlen(texto);
    if(b->tamanho + add + 1 > b->capacidade){
        while(b->tamanho + add + 1 > b->capacidade) b->capacidade *= 2;
        b->dados = realloc(b->dados, b->capacidade);
    }
    memcpy(b->dados + b->tamanho, texto, add);
    b->tamanho += add;
    b->dados[b->tamanho] = '\0';
}
static void buf_liberar(Buffer *b){ free(b->dados); }

/* strcasestr não existe em todo lugar por padrão; versão simples própria */
static char *strcasestr_local(const char *haystack, const char *needle){
    size_t hlen = strlen(haystack), nlen = strlen(needle);
    if(nlen == 0) return (char*)haystack;
    for(size_t i = 0; i + nlen <= hlen; i++){
        size_t j = 0;
        while(j < nlen && tolower((unsigned char)haystack[i+j]) == tolower((unsigned char)needle[j])) j++;
        if(j == nlen) return (char*)(haystack + i);
    }
    return NULL;
}

/* ---------------------------------------------------------------------
   leitura da requisição HTTP (linha inicial, headers, corpo)
--------------------------------------------------------------------- */
typedef struct {
    char metodo[8];
    char caminho[300];
    char *corpo;
    long corpo_tam;
} Requisicao;

static int ler_requisicao(int fd, Requisicao *req){
    size_t capacidade = BUFFER_INICIAL;
    char *buf = malloc(capacidade);
    size_t total = 0;
    long content_length = -1;
    long fim_cabecalho = -1;

    while(1){
        if(total + 4096 > capacidade){
            capacidade *= 2;
            buf = realloc(buf, capacidade);
        }
        ssize_t n = recv(fd, buf + total, capacidade - total - 1, 0);
        if(n <= 0){
            if(total == 0){ free(buf); return 0; }
            break;
        }
        total += n;
        buf[total] = '\0';

        if(fim_cabecalho < 0){
            char *marca = strstr(buf, "\r\n\r\n");
            if(marca){
                fim_cabecalho = (marca - buf) + 4;
                char *cl = strcasestr_local(buf, "Content-Length:");
                if(cl){
                    content_length = atol(cl + strlen("Content-Length:"));
                } else {
                    content_length = 0;
                }
            }
        }
        if(fim_cabecalho >= 0){
            long corpo_recebido = (long)total - fim_cabecalho;
            if(corpo_recebido >= content_length) break;
        }
        if(fim_cabecalho < 0 && total > 65536) break; /* cabeçalho absurdamente grande, desiste */
    }

    if(fim_cabecalho < 0){ free(buf); return 0; }

    /* linha inicial: METODO CAMINHO HTTP/1.1 */
    sscanf(buf, "%7s %299s", req->metodo, req->caminho);

    long corpo_tam = (long)total - fim_cabecalho;
    if(corpo_tam < 0) corpo_tam = 0;
    req->corpo = malloc(corpo_tam + 1);
    memcpy(req->corpo, buf + fim_cabecalho, corpo_tam);
    req->corpo[corpo_tam] = '\0';
    req->corpo_tam = corpo_tam;

    free(buf);
    return 1;
}

/* ---------------------------------------------------------------------
   envio de respostas HTTP (sempre com cabeçalhos CORS liberados)
--------------------------------------------------------------------- */
static void enviar_resposta(int fd, int status, const char *texto_status,
                             const char *tipo_conteudo, const char *corpo, size_t tamanho){
    Buffer b;
    buf_iniciar(&b);
    char cabecalho[512];
    snprintf(cabecalho, sizeof(cabecalho),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, texto_status, tipo_conteudo, tamanho);
    buf_add(&b, cabecalho);
    send(fd, b.dados, b.tamanho, 0);
    if(tamanho > 0) send(fd, corpo, tamanho, 0);
    buf_liberar(&b);
}

static void enviar_json(int fd, int status, const char *texto_status, const char *json){
    enviar_resposta(fd, status, texto_status, "application/json; charset=utf-8", json, strlen(json));
}

/* ---------------------------------------------------------------------
   extrai o valor de um campo "CHAVE=valor" numa das linhas de cabecalho
   do corpo (antes do marcador ---ALGO---)
--------------------------------------------------------------------- */
static char *extrair_campo(const char *cabecalhos, const char *chave){
    char alvo[64];
    snprintf(alvo, sizeof(alvo), "%s=", chave);
    const char *p = strstr(cabecalhos, alvo);
    if(!p) return strdup("");
    p += strlen(alvo);
    const char *fim = strchr(p, '\n');
    size_t len = fim ? (size_t)(fim - p) : strlen(p);
    char *valor = malloc(len + 1);
    memcpy(valor, p, len);
    valor[len] = '\0';
    tirar_quebra(valor);
    return valor;
}

/* separa o corpo em (cabecalhos, conteudo) usando o marcador --- ---
   ex: marcador = "---CONTEUDO---" */
static const char *apos_marcador(const char *corpo, const char *marcador){
    const char *p = strstr(corpo, marcador);
    if(!p) return NULL;
    p += strlen(marcador);
    if(*p == '\n') p++;
    return p;
}

/* ---------------------------------------------------------------------
   rotas: TEXTOS   (arquivos data/textos/<id>.meta e <id>.txt)
--------------------------------------------------------------------- */
static void listar_textos_json(const char *usuario, Buffer *out){
    char dir_textos[300];
    snprintf(dir_textos, sizeof(dir_textos), "data/textos/%s", usuario);
    buf_add(out, "[");
    DIR *d = opendir(dir_textos);
    int primeiro = 1;
    if(d){
        struct dirent *ent;
        while((ent = readdir(d)) != NULL){
            char *ponto = strstr(ent->d_name, ".meta");
            if(!ponto) continue;
            char id[256];
            size_t idlen = ponto - ent->d_name;
            memcpy(id, ent->d_name, idlen); id[idlen] = '\0';

            char caminho_meta[300], caminho_txt[300];
            snprintf(caminho_meta, sizeof(caminho_meta), "data/textos/%s/%s.meta", usuario, id);
            snprintf(caminho_txt, sizeof(caminho_txt), "data/textos/%s/%s.txt", usuario, id);

            char *meta = ler_arquivo_completo(caminho_meta, NULL);
            long tam_txt = 0;
            char *conteudo = ler_arquivo_completo(caminho_txt, &tam_txt);
            if(!meta || !conteudo){ free(meta); free(conteudo); continue; }

            char titulo[256] = "", data_iso[64] = "";
            sscanf(meta, "%255[^\n]\n%63[^\n]", titulo, data_iso);

            char *titulo_esc = escapar_json(titulo);
            char *conteudo_esc = escapar_json(conteudo);

            if(!primeiro) buf_add(out, ",");
            primeiro = 0;
            char item[8192 + 1024];
            snprintf(item, sizeof(item),
                "{\"id\":\"%s\",\"titulo\":\"%s\",\"conteudo\":\"%s\",\"data\":\"%s\"}",
                id, titulo_esc, conteudo_esc, data_iso);
            buf_add(out, item);

            free(meta); free(conteudo); free(titulo_esc); free(conteudo_esc);
        }
        closedir(d);
    }
    buf_add(out, "]");
}

static void criar_texto(const char *usuario, const char *corpo, Buffer *out){
    char *titulo_enc = extrair_campo(corpo, "TITULO");
    char *data_iso = extrair_campo(corpo, "DATA");
    const char *conteudo = apos_marcador(corpo, "---CONTEUDO---");
    if(!conteudo) conteudo = "";
    char *titulo = url_decode(titulo_enc);

    const char *id = gerar_id();
    char caminho_meta[300], caminho_txt[300];
    snprintf(caminho_meta, sizeof(caminho_meta), "data/textos/%s/%s.meta", usuario, id);
    snprintf(caminho_txt, sizeof(caminho_txt), "data/textos/%s/%s.txt", usuario, id);

    char meta[512];
    snprintf(meta, sizeof(meta), "%s\n%s\n", titulo, data_iso);
    escrever_arquivo(caminho_meta, meta, strlen(meta));
    escrever_arquivo(caminho_txt, conteudo, strlen(conteudo));

    char *titulo_esc = escapar_json(titulo);
    char *conteudo_esc = escapar_json(conteudo);
    char item[8192 + 1024];
    snprintf(item, sizeof(item),
        "{\"id\":\"%s\",\"titulo\":\"%s\",\"conteudo\":\"%s\",\"data\":\"%s\"}",
        id, titulo_esc, conteudo_esc, data_iso);
    buf_add(out, item);

    free(titulo_enc); free(data_iso); free(titulo); free(titulo_esc); free(conteudo_esc);
}

static void apagar_texto(const char *usuario, const char *id){
    char caminho_meta[300], caminho_txt[300];
    snprintf(caminho_meta, sizeof(caminho_meta), "data/textos/%s/%s.meta", usuario, id);
    snprintf(caminho_txt, sizeof(caminho_txt), "data/textos/%s/%s.txt", usuario, id);
    remove(caminho_meta);
    remove(caminho_txt);
}

/* ---------------------------------------------------------------------
   rotas: RECADOS   (arquivos data/recados/<id>.meta e <id>.txt)
--------------------------------------------------------------------- */
static void listar_recados_json(Buffer *out){
    buf_add(out, "[");
    DIR *d = opendir("data/recados");
    int primeiro = 1;
    if(d){
        struct dirent *ent;
        while((ent = readdir(d)) != NULL){
            char *ponto = strstr(ent->d_name, ".meta");
            if(!ponto) continue;
            char id[256];
            size_t idlen = ponto - ent->d_name;
            memcpy(id, ent->d_name, idlen); id[idlen] = '\0';

            char caminho_meta[300], caminho_txt[300];
            snprintf(caminho_meta, sizeof(caminho_meta), "data/recados/%s.meta", id);
            snprintf(caminho_txt, sizeof(caminho_txt), "data/recados/%s.txt", id);

            char *meta = ler_arquivo_completo(caminho_meta, NULL);
            char *conteudo = ler_arquivo_completo(caminho_txt, NULL);
            if(!meta || !conteudo){ free(meta); free(conteudo); continue; }

            char nome[256] = "", data_iso[64] = "";
            sscanf(meta, "%255[^\n]\n%63[^\n]", nome, data_iso);

            char *nome_esc = escapar_json(nome);
            char *msg_esc = escapar_json(conteudo);

            if(!primeiro) buf_add(out, ",");
            primeiro = 0;
            char item[8192 + 1024];
            snprintf(item, sizeof(item),
                "{\"id\":\"%s\",\"nome\":\"%s\",\"mensagem\":\"%s\",\"data\":\"%s\"}",
                id, nome_esc, msg_esc, data_iso);
            buf_add(out, item);

            free(meta); free(conteudo); free(nome_esc); free(msg_esc);
        }
        closedir(d);
    }
    buf_add(out, "]");
}

static void criar_recado(const char *corpo, Buffer *out){
    char *nome_enc = extrair_campo(corpo, "NOME");
    char *data_iso = extrair_campo(corpo, "DATA");
    const char *mensagem = apos_marcador(corpo, "---MENSAGEM---");
    if(!mensagem) mensagem = "";
    char *nome = url_decode(nome_enc);

    const char *id = gerar_id();
    char caminho_meta[300], caminho_txt[300];
    snprintf(caminho_meta, sizeof(caminho_meta), "data/recados/%s.meta", id);
    snprintf(caminho_txt, sizeof(caminho_txt), "data/recados/%s.txt", id);

    char meta[512];
    snprintf(meta, sizeof(meta), "%s\n%s\n", nome, data_iso);
    escrever_arquivo(caminho_meta, meta, strlen(meta));
    escrever_arquivo(caminho_txt, mensagem, strlen(mensagem));

    char *nome_esc = escapar_json(nome);
    char *msg_esc = escapar_json(mensagem);
    char item[8192 + 1024];
    snprintf(item, sizeof(item),
        "{\"id\":\"%s\",\"nome\":\"%s\",\"mensagem\":\"%s\",\"data\":\"%s\"}",
        id, nome_esc, msg_esc, data_iso);
    buf_add(out, item);

    free(nome_enc); free(data_iso); free(nome); free(nome_esc); free(msg_esc);
}

static void apagar_recado(const char *id){
    char caminho_meta[300], caminho_txt[300];
    snprintf(caminho_meta, sizeof(caminho_meta), "data/recados/%s.meta", id);
    snprintf(caminho_txt, sizeof(caminho_txt), "data/recados/%s.txt", id);
    remove(caminho_meta);
    remove(caminho_txt);
}

/* ---------------------------------------------------------------------
   rotas: FOTOS   (data/fotos/<id>.meta e <id>.img -- .img guarda o
   texto em base64 direto, sem decodificar, pra simplificar o C)
--------------------------------------------------------------------- */
static void listar_fotos_json(const char *usuario, Buffer *out){
    char dir_fotos[300];
    snprintf(dir_fotos, sizeof(dir_fotos), "data/fotos/%s", usuario);
    buf_add(out, "[");
    DIR *d = opendir(dir_fotos);
    int primeiro = 1;
    if(d){
        struct dirent *ent;
        while((ent = readdir(d)) != NULL){
            char *ponto = strstr(ent->d_name, ".meta");
            if(!ponto) continue;
            char id[256];
            size_t idlen = ponto - ent->d_name;
            memcpy(id, ent->d_name, idlen); id[idlen] = '\0';

            char caminho_meta[300], caminho_img[300];
            snprintf(caminho_meta, sizeof(caminho_meta), "data/fotos/%s/%s.meta", usuario, id);
            snprintf(caminho_img, sizeof(caminho_img), "data/fotos/%s/%s.img", usuario, id);

            char *meta = ler_arquivo_completo(caminho_meta, NULL);
            long tam_img = 0;
            char *img = ler_arquivo_completo(caminho_img, &tam_img);
            if(!meta || !img){ free(meta); free(img); continue; }

            char legenda[256] = "", data_iso[64] = "";
            sscanf(meta, "%255[^\n]\n%63[^\n]", legenda, data_iso);
            char *legenda_esc = escapar_json(legenda);

            if(!primeiro) buf_add(out, ",");
            primeiro = 0;
            buf_add(out, "{\"id\":\"");
            buf_add(out, id);
            buf_add(out, "\",\"legenda\":\"");
            buf_add(out, legenda_esc);
            buf_add(out, "\",\"data\":\"");
            buf_add(out, data_iso);
            buf_add(out, "\",\"imagem\":\"");
            buf_add(out, img); /* base64 puro, sem caracteres que quebrem JSON */
            buf_add(out, "\"}");

            free(meta); free(img); free(legenda_esc);
        }
        closedir(d);
    }
    buf_add(out, "]");
}

static void criar_foto(const char *usuario, const char *corpo, Buffer *out){
    char *legenda_enc = extrair_campo(corpo, "LEGENDA");
    char *data_iso = extrair_campo(corpo, "DATA");
    const char *imagem_b64 = apos_marcador(corpo, "---IMG_BASE64---");
    if(!imagem_b64) imagem_b64 = "";
    char *legenda = url_decode(legenda_enc);

    const char *id = gerar_id();
    char caminho_meta[300], caminho_img[300];
    snprintf(caminho_meta, sizeof(caminho_meta), "data/fotos/%s/%s.meta", usuario, id);
    snprintf(caminho_img, sizeof(caminho_img), "data/fotos/%s/%s.img", usuario, id);

    char meta[512];
    snprintf(meta, sizeof(meta), "%s\n%s\n", legenda, data_iso);
    escrever_arquivo(caminho_meta, meta, strlen(meta));
    escrever_arquivo(caminho_img, imagem_b64, strlen(imagem_b64));

    char *legenda_esc = escapar_json(legenda);
    buf_add(out, "{\"id\":\"");
    buf_add(out, id);
    buf_add(out, "\",\"legenda\":\"");
    buf_add(out, legenda_esc);
    buf_add(out, "\",\"data\":\"");
    buf_add(out, data_iso);
    buf_add(out, "\"}");

    free(legenda_enc); free(data_iso); free(legenda); free(legenda_esc);
}

static void apagar_foto(const char *usuario, const char *id){
    char caminho_meta[300], caminho_img[300];
    snprintf(caminho_meta, sizeof(caminho_meta), "data/fotos/%s/%s.meta", usuario, id);
    snprintf(caminho_img, sizeof(caminho_img), "data/fotos/%s/%s.img", usuario, id);
    remove(caminho_meta);
    remove(caminho_img);
}

/* ---------------------------------------------------------------------
   roteamento principal
--------------------------------------------------------------------- */
/* separa "usuario" e opcionalmente "id" de um caminho tipo
   "/api/fotos/amanda" ou "/api/fotos/amanda/6a616b2c00ce" */
static int separar_usuario_id(const char *caminho, const char *prefixo, char *usuario, size_t tam_usuario, char *id, size_t tam_id){
    if(strncmp(caminho, prefixo, strlen(prefixo)) != 0) return 0;
    const char *resto = caminho + strlen(prefixo);
    const char *barra = strchr(resto, '/');
    if(barra){
        size_t len_usuario = barra - resto;
        if(len_usuario == 0 || len_usuario >= tam_usuario) return 0;
        memcpy(usuario, resto, len_usuario);
        usuario[len_usuario] = '\0';
        strncpy(id, barra + 1, tam_id - 1);
        id[tam_id - 1] = '\0';
    } else {
        if(strlen(resto) == 0 || strlen(resto) >= tam_usuario) return 0;
        strncpy(usuario, resto, tam_usuario - 1);
        usuario[tam_usuario - 1] = '\0';
        id[0] = '\0';
    }
    return 1;
}

static void tratar_requisicao(int fd, Requisicao *req){
    Buffer out;
    buf_iniciar(&out);
    char usuario[64], id[256];

    if(strcmp(req->metodo, "OPTIONS") == 0){
        enviar_resposta(fd, 204, "No Content", "text/plain", "", 0);
        buf_liberar(&out);
        return;
    }

    /* --- FOTOS (por usuário) --- */
    if(strcmp(req->metodo, "GET") == 0 && separar_usuario_id(req->caminho, "/api/fotos/", usuario, sizeof(usuario), id, sizeof(id)) && id[0] == '\0'){
        if(!usuario_valido(usuario)){ enviar_json(fd, 403, "Forbidden", "{\"erro\":\"usuario invalido\"}"); buf_liberar(&out); return; }
        listar_fotos_json(usuario, &out);
        enviar_json(fd, 200, "OK", out.dados);
    }
    else if(strcmp(req->metodo, "POST") == 0 && separar_usuario_id(req->caminho, "/api/fotos/", usuario, sizeof(usuario), id, sizeof(id)) && id[0] == '\0'){
        if(!usuario_valido(usuario)){ enviar_json(fd, 403, "Forbidden", "{\"erro\":\"usuario invalido\"}"); buf_liberar(&out); return; }
        criar_foto(usuario, req->corpo, &out);
        enviar_json(fd, 201, "Created", out.dados);
    }
    else if(strcmp(req->metodo, "DELETE") == 0 && separar_usuario_id(req->caminho, "/api/fotos/", usuario, sizeof(usuario), id, sizeof(id)) && id[0] != '\0'){
        if(!usuario_valido(usuario)){ enviar_json(fd, 403, "Forbidden", "{\"erro\":\"usuario invalido\"}"); buf_liberar(&out); return; }
        apagar_foto(usuario, id);
        enviar_json(fd, 200, "OK", "{\"ok\":true}");
    }

    /* --- TEXTOS (por usuário) --- */
    else if(strcmp(req->metodo, "GET") == 0 && separar_usuario_id(req->caminho, "/api/textos/", usuario, sizeof(usuario), id, sizeof(id)) && id[0] == '\0'){
        if(!usuario_valido(usuario)){ enviar_json(fd, 403, "Forbidden", "{\"erro\":\"usuario invalido\"}"); buf_liberar(&out); return; }
        listar_textos_json(usuario, &out);
        enviar_json(fd, 200, "OK", out.dados);
    }
    else if(strcmp(req->metodo, "POST") == 0 && separar_usuario_id(req->caminho, "/api/textos/", usuario, sizeof(usuario), id, sizeof(id)) && id[0] == '\0'){
        if(!usuario_valido(usuario)){ enviar_json(fd, 403, "Forbidden", "{\"erro\":\"usuario invalido\"}"); buf_liberar(&out); return; }
        criar_texto(usuario, req->corpo, &out);
        enviar_json(fd, 201, "Created", out.dados);
    }
    else if(strcmp(req->metodo, "DELETE") == 0 && separar_usuario_id(req->caminho, "/api/textos/", usuario, sizeof(usuario), id, sizeof(id)) && id[0] != '\0'){
        if(!usuario_valido(usuario)){ enviar_json(fd, 403, "Forbidden", "{\"erro\":\"usuario invalido\"}"); buf_liberar(&out); return; }
        apagar_texto(usuario, id);
        enviar_json(fd, 200, "OK", "{\"ok\":true}");
    }

    /* --- RECADOS (compartilhados, sem usuário na rota) --- */
    else if(strcmp(req->metodo, "GET") == 0 && strcmp(req->caminho, "/api/recados") == 0){
        listar_recados_json(&out);
        enviar_json(fd, 200, "OK", out.dados);
    }
    else if(strcmp(req->metodo, "POST") == 0 && strcmp(req->caminho, "/api/recados") == 0){
        criar_recado(req->corpo, &out);
        enviar_json(fd, 201, "Created", out.dados);
    }
    else if(strcmp(req->metodo, "DELETE") == 0 && strncmp(req->caminho, "/api/recados/", 13) == 0){
        apagar_recado(req->caminho + 13);
        enviar_json(fd, 200, "OK", "{\"ok\":true}");
    }
    else {
        enviar_json(fd, 404, "Not Found", "{\"erro\":\"rota nao encontrada\"}");
    }

    buf_liberar(&out);
}

/* ---------------------------------------------------------------------
   main: cria o socket, escuta e atende uma conexão por vez
--------------------------------------------------------------------- */
int main(void){
    srand((unsigned)time(NULL));

    garantir_diretorio("data");
    garantir_diretorio("data/fotos");
    garantir_diretorio("data/fotos/amanda");
    garantir_diretorio("data/fotos/caetano");
    garantir_diretorio("data/textos");
    garantir_diretorio("data/textos/amanda");
    garantir_diretorio("data/textos/caetano");
    garantir_diretorio("data/recados");

    int servidor_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(servidor_fd < 0){ perror("socket"); return 1; }

    int opt = 1;
    setsockopt(servidor_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in endereco;
    memset(&endereco, 0, sizeof(endereco));
    endereco.sin_family = AF_INET;
    endereco.sin_addr.s_addr = INADDR_ANY;
    endereco.sin_port = htons(PORTA);

    if(bind(servidor_fd, (struct sockaddr*)&endereco, sizeof(endereco)) < 0){
        perror("bind");
        return 1;
    }
    if(listen(servidor_fd, 16) < 0){
        perror("listen");
        return 1;
    }

    printf("servidor de memorias rodando em http://localhost:%d\n", PORTA);
    printf("dados sendo salvos em ./data\n");

    while(1){
        struct sockaddr_in cliente_end;
        socklen_t tam = sizeof(cliente_end);
        int cliente_fd = accept(servidor_fd, (struct sockaddr*)&cliente_end, &tam);
        if(cliente_fd < 0) continue;

        Requisicao req;
        memset(&req, 0, sizeof(req));
        if(ler_requisicao(cliente_fd, &req)){
            tratar_requisicao(cliente_fd, &req);
            free(req.corpo);
        }
        close(cliente_fd);
    }

    close(servidor_fd);
    return 0;
}
