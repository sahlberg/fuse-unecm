/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */

struct ecm *ecm_open_file(int dir_fd, const char *file);
void ecm_close_file(struct ecm *e);
ssize_t ecm_read(struct ecm *ecm, char *buf, off_t offset, size_t len);
size_t ecm_get_file_size(struct ecm *ecm);
