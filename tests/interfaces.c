#include <stdio.h>
#include "select_interface.h"

int main(void)
{
    const char *name = list_and_select_interface();
    if (!name) return 1;
    printf("SELECTED:%s\n", name);
    return 0;
}
