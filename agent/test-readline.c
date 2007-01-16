
#include <string.h>

#include <glib.h>

#include <readline.h>

/* this overrides libc read() -- is this reliable? */
int
read (int fd, void *buf, size_t count)
{
  static int offset = 0;
  gchar *line = "test\n";

  g_assert (count == 1);
  g_assert (offset < 5);
  * (gchar *) buf = line[offset++];
  return 1;
}

int
main (void)
{
  gchar *line;

  line = readline (0);
  g_assert (0 == strcmp (line, "test"));
  return 0;
}
