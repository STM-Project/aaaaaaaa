#ifndef BOOT_HTTP_H
#define BOOT_HTTP_H

enum http_result { HTTP_OTHER, HTTP_PAGE, HTTP_INSTALLED };
enum http_result http_serve(unsigned link, const char *update_key);

#endif
