/* Standalone harness for gas_assemble_buffer().
   Usage: gas_embed_test <input.s> <output.o>
   Reads the whole .s file into a malloc'd buffer and assembles it in-process.
   Prints the return code and exits with it (so a non-zero rc from a fatal
   assembler condition is observable without the process being killed).  */

#include <stdio.h>
#include <stdlib.h>

extern int gas_assemble_buffer (const char *asm_buf, unsigned long len,
				const char *out_obj);

int
main (int argc, char **argv)
{
  if (argc != 3)
    {
      fprintf (stderr, "usage: %s <input.s> <output.o>\n", argv[0]);
      return 2;
    }

  FILE *f = fopen (argv[1], "rb");
  if (!f)
    {
      perror (argv[1]);
      return 2;
    }
  if (fseek (f, 0, SEEK_END) != 0)
    {
      perror ("fseek");
      fclose (f);
      return 2;
    }
  long sz = ftell (f);
  if (sz < 0)
    {
      perror ("ftell");
      fclose (f);
      return 2;
    }
  rewind (f);

  char *buf = malloc ((size_t) sz + 1);
  if (!buf)
    {
      fprintf (stderr, "out of memory\n");
      fclose (f);
      return 2;
    }
  size_t got = fread (buf, 1, (size_t) sz, f);
  fclose (f);
  buf[got] = '\0';

  int rc = gas_assemble_buffer (buf, (unsigned long) got, argv[2]);
  fprintf (stderr, "[harness] gas_assemble_buffer returned %d\n", rc);
  free (buf);
  return rc == 0 ? 0 : 1;
}
