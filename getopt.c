#include "types.h"
#include "user.h"
int opts = 0;

/***
 * Simple implementation of getopt in getopt.h of linux
 * it accepts only a:b style of options and works same with the original
 * global optind variable always indicates the next index of args when it hits any options
 */

struct opt
{
    char name;
    int has_arg;
};

int optind = 0;
static int optsize = -1;
static struct opt *options = 0;

int getopts(int argc, char *argv[], const char *optstring)
{
    int i;
    // initialize options to recognize after
    if (optsize == -1)
    {
        int len = strlen(optstring);
        int size = 0;
        // loop option strings
        for (i = 0; i < len; i++)
        {
            char c = optstring[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            {
                size++;
            }
        }

        if (size <= 0)
        {
            return -1;
        }

        // save options
        optsize = size;
        options = (struct opt *)malloc(sizeof(struct opt) * size);

        size = 0;

        // and check it accepts args or etc
        for (i = 0; i < len; i++)
        {
            char c = optstring[i];
            int has_arg = i + 1 < len && optstring[i + 1] ? optstring[i + 1] == ':' : 0;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            {
                options[size].name = c;
                options[size++].has_arg = has_arg;
            }
        }

        optind = 0;
    }

    // if initialized, start looping args from current index(optind) and match option
    for (i = optind; i < argc; i++)
    {

        if (argv[i][0] != '-' || !((argv[i][1] >= 'a' && argv[i][1] <= 'z') || (argv[i][1] >= 'A' && argv[i][1] <= 'Z')))
            continue;

        for (int j = 0; j < optsize; j++)
        {
            // option matches
            if (options[j].name == argv[i][1])
            {
                int has_arg = i + 1 < argc;
                optind = i + 1;
                // if option required additional argument(option with :)
                if (options[j].has_arg)
                {
                    // if no argument, then throw error
                    if (!has_arg)
                    {
                        printf(2, "unmatched option: -%c\n", options[j].name);
                        goto clean;
                    }
                }
                return options[j].name;
            }
        }
    }

clean:
    free(options);
    optsize = -1;
    return -1;
}
