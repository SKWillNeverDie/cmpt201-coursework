#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RED "\e[9;31m"
#define GRN "\e[0;32m"
#define CRESET "\e[0m"

#define handle_error(msg)                                                      \
  do {                                                                         \
    perror(msg);                                                               \
    exit(EXIT_FAILURE);                                                        \
  } while (0)

size_t read_all_bytes(const char *filename, void *buffer, size_t buffer_size) {
  FILE *file = fopen(filename, "rb");
  if (!file) {
    handle_error("Error opening file");
  }

  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (file_size < 0) {
    handle_error("Error getting file size");
  }

  if ((size_t)file_size > buffer_size) {
    handle_error("File size is too large");
  }

  if (fread(buffer, 1, file_size, file) != (size_t)file_size) {
    handle_error("Error reading file");
  }

  fclose(file);
  return (size_t)file_size;
}

void print_file(const char *filename, const char *color) {
  FILE *file = fopen(filename, "r");
  if (!file) {
    handle_error("Error opening file");
  }

  printf("%s", color);
  char line[256];
  while (fgets(line, sizeof(line), file)) {
    printf("%s", line);
  }
  fclose(file);
  printf(CRESET);
}

int verify(const char *message_path, const char *sign_path, EVP_PKEY *pubkey);

int main() {
  // File paths
  const char *message_files[] = {"message1.txt", "message2.txt",
                                 "message3.txt"};
  const char *signature_files[] = {"signature1.sig", "signature2.sig",
                                   "signature3.sig"};

  FILE *pkfile = fopen("public_key.pem", "r");
  if (!pkfile) {
    handle_error("Error opening public_key.pem");
  }

  EVP_PKEY *pubkey = PEM_read_PUBKEY(pkfile, NULL, NULL, NULL);
  fclose(pkfile);

  if (!pubkey) {
    fprintf(stderr, "Error reading public key\n");
    ERR_print_errors_fp(stderr);
    exit(EXIT_FAILURE);
  }

  // Verify each message
  for (int i = 0; i < 3; i++) {
    printf("... Verifying message %d ...\n", i + 1);
    int result = verify(message_files[i], signature_files[i], pubkey);

    if (result < 0) {
      printf("Unknown authenticity of message %d\n", i + 1);
      print_file(message_files[i], CRESET);
    } else if (result == 0) {
      printf("Do not trust message %d!\n", i + 1);
      print_file(message_files[i], RED);
    } else {
      printf("Message %d is authentic!\n", i + 1);
      print_file(message_files[i], GRN);
    }
    printf("\n");
  }

  EVP_PKEY_free(pubkey);

  return 0;
}

/*
    Verify that the file `message_path` matches the signature `sign_path`
    using `pubkey`.
    Returns:
         1: Message matches signature
         0: Signature did not verify successfully
        -1: Message is does not match signature
*/
int verify(const char *message_path, const char *sign_path, EVP_PKEY *pubkey) {
#define MAX_FILE_SIZE 512
  unsigned char message[MAX_FILE_SIZE];
  unsigned char signature[MAX_FILE_SIZE];

  size_t message_len = read_all_bytes(message_path, message, sizeof(message));
  size_t signature_len =
      read_all_bytes(sign_path, signature, sizeof(signature));

  EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
  if (!mdctx) {
    fprintf(stderr, "Error creating digest context\n");
    ERR_print_errors_fp(stderr);
    return -1;
  }

  if (EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, pubkey) != 1) {
    fprintf(stderr, "Error in EVP_DigestVerifyInit\n");
    ERR_print_errors_fp(stderr);
    EVP_MD_CTX_free(mdctx);
    return -1;
  }

  if (EVP_DigestVerifyUpdate(mdctx, message, message_len) != 1) {
    fprintf(stderr, "Error in EVP_DigestVerifyUpdate\n");
    ERR_print_errors_fp(stderr);
    EVP_MD_CTX_free(mdctx);
    return -1;
  }

  int ret = EVP_DigestVerifyFinal(mdctx, signature, signature_len);

  EVP_MD_CTX_free(mdctx);

  if (ret == 1) {
    return 1;
  } else if (ret == 0) {
    return 0;
  } else {
    fprintf(stderr, "Error in EVP_DigestVerifyFinal\n");
    ERR_print_errors_fp(stderr);
    return -1;
  }
}
