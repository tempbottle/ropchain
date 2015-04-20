#include "rop.h"

int rop_chain(unsigned char **chain, unsigned char *binary, struct Arg *arg)
{
    struct Node *root;
    struct Segment *text;
    int result;
    text = elf_parse(binary);
    if(!text)
    {
        fprintf(stderr ,"parse elf failed.\n");
        return -1;
    }
    root = (struct Node *)malloc(sizeof(struct Node));
    if(!root)
    {
        fprintf(stderr ,"malloc failed.\n");
        return -1;
    }
    struct Gadget *head;
    head = (struct Gadget *)malloc(sizeof(struct Gadget));
    if(!head)
    {
        fprintf(stderr ,"malloc failed.\n");
        return -1;
    }
    *chain = (unsigned char *)malloc(sizeof(unsigned char));
    if(!*chain)
    {
        fprintf(stderr ,"malloc failed.\n");
        return -1;
    }
    rop_parse_gadgets(root, binary, text, arg);
    result = rop_chain_execve(root, head, arg);
    if(!result)
    {
        result = rop_chain_list_traverse(head, chain);
    }
    rop_chain_list_free(head);
    tree_free(root);
    return result;
}

int rop_parse_gadgets(struct Node *root, unsigned char *binary, struct Segment *text, struct Arg *arg)
{
    size_t count;
    csh handle;
    cs_insn *insn;
    char gadget_string[MaxGadgetLen];
    int total_gadget = 0;
    size_t i,j,k;

    tree_init(root);

    if(cs_open(CS_ARCH_X86, CS_MODE_32, &handle) != CS_ERR_OK)
    {
        return -1;
    }
    for(i = 0; i < text->memsz - MaxGadgetByte; i++)
    {
        count = cs_disasm_ex(handle, binary + i, MaxGadgetByte, text->vaddr + i, 0, &insn);
        if(count > 0)
        {
            strcpy(gadget_string, "");
            for(j = 0; j < count; j++)
            {
                /* Drop the gadgets start with ret */
                if(!strcmp(insn[0].mnemonic, "ret"))
                {
                    break;
                }
                /* Drop jump/call gadgets */
                else if(strchr(insn[j].mnemonic, 'j')
                || strstr(insn[j].mnemonic, "call") || strstr(insn[j].mnemonic, "push"))
                {
                    break;
                }
                /* Ret-type gadgets */
                else if(!strcmp(insn[j].mnemonic, "ret") && j)
                {
                    if(strlen(insn[j].op_str) == 0)
                    {
                        total_gadget++;
                        for(k = 0; k < j; k++)
                        {
                            if(arg->print && strlen(gadget_string)
                            + strlen(insn[k].mnemonic) + strlen(insn[k].op_str) + 7 < MaxGadgetLen)
                            {
                                strcat(gadget_string, insn[k].mnemonic);
                                if(strlen(insn[k].op_str) > 0)
                                {
                                    strcat(gadget_string, " ");
                                    strcat(gadget_string, insn[k].op_str);
                                }
                                strcat(gadget_string, " ; ");
                            }
                        }
                        /* tree build */
                        tree_build(root, 0, insn, j+1);
                        if(arg->print && strlen(gadget_string) + 3 < MaxGadgetLen)
                        {
                            strcat(gadget_string, "ret");
                            /* print all gadgets */
                            printf("%d\t0x0%x:\t%s\n", j+1, text->vaddr + i, gadget_string);
                        }
                    }
                    else
                    {
                        i += j;
                    }
                    strcpy(gadget_string, "");
                    break;
                }
                else if(j == 0 && !strcmp(insn[j].mnemonic, "int") && !strcmp(insn[j].op_str, "0x80"))
                {
                    total_gadget++;
                    /* tree build */
                    tree_build(root, 0, insn, j+1);
                    if(arg->print == 1)
                    {
                        /* print int80 gadgets */
                        printf("%d\t0x0%"PRIx64":\tint 0x80\n", j+1, insn[j].address);
                    }
                    break;
                }
            }
            cs_free(insn, count);
        }
    }
    printf("Gadget find = %d\n",total_gadget);
    cs_close(&handle);
    return 0;
}

int rop_chain_execve(struct Node *root, struct Gadget *head, struct Arg *arg)
{
    struct API *api;
    rop_build_api(root, &api, arg);

    printf("\n--- Start chain *execve(\"/bin/sh\")* gadgets ---\n\n");
    rop_chain_list_init(head);
    rop_write_memory_gadget(head, api, 0x080efff0, 0x6e69622f);
    rop_write_memory_gadget(head, api, 0x080efff4, 0x68732f2f);
    rop_write_memory_gadget(head, api, 0x080efff8, 0);

    rop_write_register_gadget(api, "ebx", 0x080efff0);
    rop_write_register_gadget(api, "ecx", 0x080efff8);
    rop_write_register_gadget(api, "edx", 0x080efff8);
    rop_chain_write_register_gadget(head, api);

    rop_zero_register_gadget(head, api, "eax");
    rop_add_register_gadget(head, api, "eax", 11);
    rop_interrupt_gadget(head, api);

    rop_end_api(api);
    return 0;
}

void rop_build_api(struct Node *root, struct API **api, struct Arg *arg)
{
    *api = (struct API *)malloc(sizeof(struct API));
    if(!*api)
    {
        fprintf(stderr ,"malloc failed.\n");
        exit(-1);
    }
    (*api)->result_writeMEM = rop_build_write_memory_gadget(root, &((*api)->writeMEM), arg);
    (*api)->result_readMEM = rop_build_read_memory_gadget(root, &((*api)->readMEM), arg);
    (*api)->result_writeREG = rop_build_write_register_gadget(root, &((*api)->writeREG), arg);
    (*api)->result_xchgREG = rop_build_xchg_register_gadget(root, &((*api)->xchgREG), arg);
    (*api)->result_zeroREG = rop_build_zero_register_gadget(root, &((*api)->zeroREG), arg);
    (*api)->result_addREG = rop_build_add_register_gadget(root, &((*api)->addREG), arg);
    (*api)->result_INT = rop_build_interrupt_gadget(root, &((*api)->INT), arg);
}

void rop_end_api(struct API *api)
{
    rop_chain_list_free(api->writeMEM);
    rop_chain_list_free(api->readMEM);
    rop_chain_list_free(api->writeREG);
    rop_chain_list_free(api->zeroREG);
    rop_chain_list_free(api->addREG);
    rop_chain_list_free(api->INT);
}

int rop_chain_write_register_gadget(struct Gadget *head, struct API *api)
{
    int i, j, k;
    struct Gadget *writeREG = api->writeREG;
    struct Gadget *temp = writeREG;
    char part_target_write[20][10];
    char string_padding[20];
    int part_target_write_no = 0;
    int gw_ttw, gw_ptw;
    /* find last one and ptw */
    for(i = 0; i < writeREG->total_target_write_no; i++)
    {
        temp = temp->next->next;
        while(temp->address == 0)
        {
            temp->prev->order = -1;
            temp = temp->next->next;
        }
        temp = temp->prev;
        gw_ttw = 0;
        for(j = 1; j < temp->gadget_write_no; j++)
        {
            for(k = 0; k < writeREG->total_target_write_no; k++)
            {
                if(!strcmp(temp->gadget_write[j], writeREG->total_target_write[k]))
                {
                    gw_ttw++;
                }
            }
        }
        if(gw_ttw == 0)
        {
            temp->order = 20;
        }
        else
        {
            temp->order = -1;
            strcpy(part_target_write[part_target_write_no++], temp->target_write);
        }
        temp = temp->next;
    }
    /* sort ptw order */
    temp = writeREG;
    for(i = 0; i < part_target_write_no; i++)
    {
        temp = temp->next->next;
        while(temp->address == 0 || temp->prev->order == 20)
        {
            temp = temp->next->next;
        }
        temp = temp->prev;
        gw_ptw = 0;
        for(j = 1; j < temp->gadget_write_no; j++)
        {
            for(k = 0; k < part_target_write_no; k++)
            {
                if(!strcmp(temp->gadget_write[j], part_target_write[k]))
                {
                    gw_ptw++;
                }
            }
        }
        temp->order = 19 - gw_ptw;
        temp = temp->next;
    }
    /* Add to list */
    for(i = 0; i <= 20; i++)
    {
        temp = writeREG;
        for(j = 0; j < writeREG->total_target_write_no; j++)
        {
            temp = temp->next->next;
            while(temp->address == 0)
            {
                temp = temp->next->next;
            }
            temp = temp->prev;
            if(temp->order == i)
            {
                rop_chain_list_add(head, temp->address, temp->string, 1);
                rop_chain_list_add(head, temp->next->address, temp->next->string, 1);
                if(temp->padding > 0)
                {
                    sprintf(string_padding, "padding*%d", temp->padding);
                    rop_chain_list_add(head, 0x41414141, string_padding, 1);
                }
            }
            temp = temp->next;
        }
    }
    return 0;
}

int rop_write_memory_gadget(struct Gadget *head, struct API *api, unsigned int dest, unsigned int value)
{
    struct Gadget *temp;
    char string_value[4];
    char string_padding[20];
    if(api->result_writeMEM == -1)
    {
        printf("X: Can't find writeMEM gadget to do this operation.\n");
        exit(-1);
    }
    temp = api->writeMEM->next;
    /* bypass xor gadget */
    if(value == 0)
    {
        rop_chain_list_add(head, temp->address, temp->string, 1);
        if(temp->padding > 0)
        {
            sprintf(string_padding, "padding*%d", temp->padding);
            rop_chain_list_add(head, 0x41414141, string_padding, 1);
        }
        temp = temp->next;
    }
    else
    {
        temp = temp->next;
        rop_chain_list_add(head, temp->address, temp->string, 1);
        memcpy(string_value, &value, 4);
        rop_chain_list_add(head, value, string_value, 1);
        if(temp->padding > 0)
        {
            sprintf(string_padding, "padding*%d", temp->padding);
            rop_chain_list_add(head, 0x41414141, string_padding, 1);
        }
    }
    /* write dest */
    temp = temp->next;
    rop_chain_list_add(head, temp->address, temp->string, 1);
    rop_chain_list_add(head, dest, "dest", 1);
    if(temp->padding > 0)
    {
        sprintf(string_padding, "padding*%d", temp->padding);
        rop_chain_list_add(head, 0x41414141, string_padding, 1);
    }
    /* move value to dest */
    temp = temp->next;
    rop_chain_list_add(head, temp->address, temp->string, 1);
    if(temp->padding > 0)
    {
        sprintf(string_padding, "padding*%d", temp->padding);
        rop_chain_list_add(head, 0x41414141, string_padding, 1);
    }
    return 0;
}

int rop_write_register_gadget(struct API *api, char *dest, unsigned int value)
{
    struct Gadget *temp;
    temp = api->writeREG->next;
    while(temp)
    {
        if(strstr(temp->target_write ,dest))
        {
            temp = temp->next;
            temp->address = value;
            strcpy(api->writeREG->total_target_write[api->writeREG->total_target_write_no++], dest);
            return 0;
        }
        temp = temp->next;
    }
    printf("X: Can't find writeREG gadget to do this operation.\n");
    exit(-1);
}

int rop_xchg_register_gadget(struct Gadget *head, struct API *api, char *op1, char *op2)
{
    struct Gadget *temp;
    char gadget_string[MaxGadgetLen] = "";
    char gadget_string2[MaxGadgetLen] = "";
    char string_padding[20];
    temp = api->xchgREG->next;
    sprintf(gadget_string, "xchg %s, %s", op1, op2);
    sprintf(gadget_string2, "xchg %s, %s", op2, op1);
    while(temp)
    {
        if(strstr(temp->string ,gadget_string) || strstr(temp->string ,gadget_string2))
        {
            rop_chain_list_add(head, temp->address, temp->string, 1);
            if(temp->padding > 0)
            {
                sprintf(string_padding, "padding*%d", temp->padding);
                rop_chain_list_add(head, 0x41414141, string_padding, 1);
            }
            return 0;
        }
        temp = temp->next;
    }
    printf("X: Can't find xchgREG gadget to do this operation.\n");
    exit(-1);
}

int rop_read_memory_gadget(struct Gadget *head, struct API *api, char *dest, unsigned int src)
{
    struct Gadget *temp;
    char string_padding[20];
    temp = api->readMEM->next;
    while(temp)
    {
        if(!strcmp(temp->target_write, dest))
        {
            if(strcmp(temp->next->string, "null"))
            {
                rop_chain_list_add(head, temp->next->address, temp->next->string, 1);
                rop_chain_list_add(head, src, "address", 1);
                if(temp->next->padding > 0)
                {
                    sprintf(string_padding, "padding*%d", temp->next->padding);
                    rop_chain_list_add(head, 0x41414141, string_padding, 1);
                }
                rop_chain_list_add(head, temp->address, temp->string, 1);
                if(temp->padding > 0)
                {
                    sprintf(string_padding, "padding*%d", temp->padding);
                    rop_chain_list_add(head, 0x41414141, string_padding, 1);
                }
                return 0;
            }
        }
        temp = temp->next->next;
    }
    printf("X: Can't find readMEM gadget to do this operation.\n");
    exit(-1);
}

int rop_zero_register_gadget(struct Gadget *head, struct API *api, char *dest)
{
    struct Gadget *temp;
    char string_padding[20];
    temp = api->zeroREG->next;
    while(temp)
    {
        if(strstr(temp->target_write ,dest))
        {
            rop_chain_list_add(head, temp->address, temp->string, 1);
            if(temp->padding > 0)
            {
                sprintf(string_padding, "padding*%d", temp->padding);
                rop_chain_list_add(head, 0x41414141, string_padding, 1);
            }
            return 0;
        }
        temp = temp->next;
    }
    printf("X: Can't find zeroREG gadget to do this operation.\n");
    exit(-1);
}

int rop_add_register_gadget(struct Gadget *head, struct API *api, char *dest, unsigned int value)
{
    unsigned int i;
    char string_padding[20];
    struct Gadget *temp;
    temp = api->addREG->next;
    while(temp)
    {
        if(strstr(temp->target_write ,dest))
        {
            for(i = 0; i < value; i++)
            {
                rop_chain_list_add(head, temp->address, temp->string, 1);
                if(temp->padding > 0)
                {
                    sprintf(string_padding, "padding*%d", temp->padding);
                    rop_chain_list_add(head, 0x41414141, string_padding, 1);
                }
            }
            return 0;
        }
        temp = temp->next;
    }
    printf("X: Can't find addREG gadget to do this operation.\n");
    exit(-1);
}

int rop_interrupt_gadget(struct Gadget *head, struct API *api)
{
    if(api->result_INT == -1)
    {
        printf("X: Can't find INT gadget to do this operation.\n");
        exit(-1);
    }
    rop_chain_list_add(head, api->INT->next->address, api->INT->next->string, 1);
    return 0;
}

int rop_build_write_memory_gadget(struct Node *root, struct Gadget **writeMEM, struct Arg *arg)
{
    struct Node *temp,*mov_temp;
    char gadget_string[MaxGadgetLen] = "";
    char regexp_string[MaxRegExpLen] = "";
    char op[2][4];
    int i, depth, restart;
    int valid;
    printf("\n--- Build WriteMem Gadgets ---\n");
    while(true)
    {
        restart = 0;
        *writeMEM = (struct Gadget *)malloc(sizeof(struct Gadget));
        if(!*writeMEM)
        {
            fprintf(stderr ,"malloc failed.\n");
            exit(-1);
        }
        rop_chain_list_init(*writeMEM);

        /* find mov gadget */
        strcpy(regexp_string, "mov dword ptr .e[abcds][xip]], e[abcds][xip]");
        for(depth = 1; depth < arg->depth; depth++)
        {
            memset(gadget_string, 0, MaxGadgetLen);
            mov_temp = tree_search(root, regexp_string, gadget_string, depth, arg);
            if(mov_temp)
            {
                printf(" O| 0x%08x -> Find MOV Gadget \"%s\"\n", mov_temp->address, gadget_string);
                break;
            }
            else if(depth == arg->depth-1)
            {
                if(arg->verbose)
                {
                    printf(" X| Can't find WriteMEM gadget.\n");
                }
                return -1;
            }
        }
        strncpy(op[0], &gadget_string[15], 3);
        strncpy(op[1], &gadget_string[21], 3);
        op[0][3] = 0;
        op[1][3] = 0;
        if(!strcmp(op[0], "esp") || !strcmp(op[1], "esp"))
        {
            printf(" X| Can't use esp gadget. Try to find other mov gadget\n");
            mov_temp->vaild = 0;
            continue;
        }
        valid = rop_chain_list_add(*writeMEM, mov_temp->address, gadget_string, 1);
        if(valid == -1)
        {
            mov_temp->vaild = 0;
            continue;
        }
        /* find pop e_x gadget */
        for(i = 0; i < 2; i++)
        {
            strcpy(regexp_string, "^pop ___$");
            strncpy(&regexp_string[5], op[i], 3);
            for(depth = 1; depth < arg->depth; depth++)
            {
                memset(gadget_string, 0, MaxGadgetLen);
                temp = tree_search(root, regexp_string, gadget_string, depth, arg);
                if(temp)
                {
                    printf(" O| 0x%08x -> Find POP Gadget \"%s\"\n", temp->address, gadget_string);
                    break;
                }
                else if(depth == arg->depth-1)
                {
                    if(arg->verbose)
                    {
                        printf(" X| Can't find gadget \"pop %s\" Try to find other mov gadget\n", op[i]);
                    }
                    mov_temp->vaild = 0;
                    rop_chain_list_free(*writeMEM);
                    restart = 1;
                    break;
                }
            }
            if(!restart)
            {
                valid = rop_chain_list_add(*writeMEM, temp->address, gadget_string, 0);
                if(valid == -1)
                {
                    temp->vaild = 0;
                    restart = 1;
                    break;
                }
            }
            else
            {
                break;
            }
        }
        if(restart)
        {
            continue;
        }

        /* find xor e_x gadget */
        strcpy(regexp_string, "xor ___, ___");
        strncpy(&regexp_string[4], op[1], 3);
        strncpy(&regexp_string[9], op[1], 3);
        for(depth = 1; depth < arg->depth; depth++)
        {
            memset(gadget_string, 0, MaxGadgetLen);
            temp = tree_search(root, regexp_string, gadget_string, depth, arg);
            if(temp)
            {
                printf(" O| 0x%08x -> Find XOR Gadget \"%s\"\n", temp->address, gadget_string);
                break;
            }
            else if(depth == arg->depth-1)
            {
                if(arg->verbose)
                {
                    printf(" X| Can't find gadget \"%s\" Try to find other mov gadget\n", regexp_string);
                }
                mov_temp->vaild = 0;
                rop_chain_list_free(*writeMEM);
                restart = 1;
                break;
            }
        }
        if(restart)
        {
            continue;
        }
        else
        {
            valid = rop_chain_list_add(*writeMEM, temp->address, gadget_string, 0);
            if(valid == -1)
            {
                temp->vaild = 0;
                continue;
            }
        }
        break;
    }
    return 0;
}

int rop_build_read_memory_gadget(struct Node *root, struct Gadget **readMEM, struct Arg *arg)
{
    struct Node *temp,*mov_temp;
    char mov_string[MaxGadgetLen] = "";
    char pop_string[MaxGadgetLen] = "";
    char regexp_string[MaxRegExpLen] = "";
    char *dst_op[4] = {"eax", "ebx", "ecx", "edx"};
    char src_op[4];
    int i, depth, restart;
    int valid;
    printf("\n--- Build ReadMem Gadgets ---\n");
    while(true)
    {
        restart = 0;
        *readMEM = (struct Gadget *)malloc(sizeof(struct Gadget));
        if(!*readMEM)
        {
            fprintf(stderr ,"malloc failed.\n");
            exit(-1);
        }
        rop_chain_list_init(*readMEM);

        /* find mov gadget */
        for(i = 0; i < 4; i++)
        {
            strcpy(regexp_string, "mov ___, dword ptr .e[abcds][xip]]");
            strncpy(&regexp_string[4], dst_op[i], 3);
            for(depth = 1; depth < arg->depth; depth++)
            {
                memset(mov_string, 0, MaxGadgetLen);
                mov_temp = tree_search(root, regexp_string, mov_string, depth, arg);
                if(mov_temp)
                {
                    printf(" O| 0x%08x -> Find MOV Gadget \"%s\"\n", mov_temp->address, mov_string);
                    break;
                }
                else if(depth == arg->depth-1)
                {
                    if(arg->verbose)
                    {
                        printf(" X| Can't find gadget \"mov %s, dword ptr reg\"\n", dst_op[i]);
                    }
                    restart = 1;
                    break;
                }
            }
            if(restart == 1)
            {
                restart = 0;
                continue;
            }
            strncpy(src_op, &mov_string[20], 3);
            src_op[3] = 0;
            if(!strcmp(src_op, "esp"))
            {
                printf(" X| Can't use esp gadget. Try to find other mov gadget\n");
                mov_temp->vaild = 0;
                i--;
                continue;
            }
            /* find pop e_x gadget */
            strcpy(regexp_string, "^pop ___$");
            strncpy(&regexp_string[5], src_op, 3);
            for(depth = 1; depth < arg->depth; depth++)
            {
                memset(pop_string, 0, MaxGadgetLen);
                temp = tree_search(root, regexp_string, pop_string, depth, arg);
                if(temp)
                {
                    printf(" O| 0x%08x -> Find POP Gadget \"%s\"\n", temp->address, pop_string);
                    break;
                }
                else if(depth == arg->depth-1)
                {
                    if(arg->verbose)
                    {
                        printf(" X| Can't find gadget \"pop %s\" Try to find other mov gadget\n", src_op);
                    }
                    mov_temp->vaild = 0;
                    restart = 1;
                    i--;
                    break;
                }
            }
            if(!restart)
            {
                valid = rop_chain_list_add(*readMEM, mov_temp->address, mov_string, 1);
                if(valid == -1)
                {
                    mov_temp->vaild = 0;
                    i--;
                    continue;
                }
                valid = rop_chain_list_add(*readMEM, temp->address, pop_string, 1);
                if(valid == -1)
                {
                    rop_chain_list_add(*readMEM, 0, "null", 1);
                    temp->vaild = 0;
                    i--;
                    continue;
                }
            }
        }
        break;
    }
    return 0;
}

int rop_build_write_register_gadget(struct Node *root, struct Gadget **writeREG, struct Arg *arg)
{
    int valid;
    struct Node *temp;
    char gadget_string[MaxGadgetLen] = "";
    char regexp_string[MaxRegExpLen] = "";
    char *op[4] = {"eax", "ebx", "ecx", "edx"};
    int i, depth, restart;
    printf("\n--- Build WriteREG Gadgets ---\n");
    *writeREG = (struct Gadget *)malloc(sizeof(struct Gadget));
    if(!*writeREG)
    {
        fprintf(stderr ,"malloc failed.\n");
        exit(-1);
    }
    rop_chain_list_init(*writeREG);

    /* find pop e_x gadget */
    for(i = 0; i < 4; i++)
    {
        restart = 0;
        strcpy(regexp_string, "^pop ___$");
        strncpy(&regexp_string[5], op[i], 3);
        for(depth = 1; depth < arg->depth; depth++)
        {
            memset(gadget_string, 0, MaxGadgetLen);
            temp = tree_search(root, regexp_string, gadget_string, depth, arg);
            if(temp)
            {
                printf(" O| 0x%08x -> Find POP Gadget \"%s\"\n", temp->address, gadget_string);
                break;
            }
            else if(depth == arg->depth-1)
            {
                if(arg->verbose)
                {
                    printf(" X| Can't find gadget \"pop %s\"\n", op[i]);
                }
                restart = 1;
                break;
            }
        }
        if(restart == 1)
        {
            restart = 0;
            continue;
        }
        valid = rop_chain_list_add(*writeREG, temp->address, gadget_string, 1);
        if(valid == -1)
        {
            temp->vaild = 0;
            i--;
            continue;
        }
        valid = rop_chain_list_add(*writeREG, 0x00000000, "value", 1);
    }
    return 0;
}

int rop_build_zero_register_gadget(struct Node *root, struct Gadget **zeroREG, struct Arg *arg)
{
    int valid;
    struct Node *temp;
    char gadget_string[MaxGadgetLen] = "";
    char regexp_string[MaxRegExpLen] = "";
    char *op[4] = {"eax", "ebx", "ecx", "edx"};
    int i, depth, restart;
    printf("\n--- Build ZeroREG Gadgets ---\n");
    *zeroREG = (struct Gadget *)malloc(sizeof(struct Gadget));
    if(!*zeroREG)
    {
        fprintf(stderr ,"malloc failed.\n");
        exit(-1);
    }
    rop_chain_list_init(*zeroREG);

    /* find xor e_x gadget */
    for(i = 0; i < 4; i++)
    {
        restart = 0;
        strcpy(regexp_string, "xor ___, ___");
        strncpy(&regexp_string[4], op[i], 3);
        strncpy(&regexp_string[9], op[i], 3);
        for(depth = 1; depth < arg->depth; depth++)
        {
            memset(gadget_string, 0, MaxGadgetLen);
            temp = tree_search(root, regexp_string, gadget_string, depth, arg);
            if(temp)
            {
                printf(" O| 0x%08x -> Find XOR Gadget \"%s\"\n", temp->address, gadget_string);
                break;
            }
            else if(depth == arg->depth-1)
            {
                if(arg->verbose)
                {
                    printf(" X| Can't find gadget \"%s\"\n", regexp_string);
                }
                restart = 1;
                break;
            }
        }
        if(restart)
        {
            continue;
        }
        valid = rop_chain_list_add(*zeroREG, temp->address, gadget_string, 1);
        if(valid == -1)
        {
            temp->vaild = 0;
            i--;
            continue;
        }
    }
    return 0;
}

int rop_build_xchg_register_gadget(struct Node *root, struct Gadget **xchgREG, struct Arg *arg)
{
    int valid;
    struct Node *temp;
    char gadget_string[MaxGadgetLen] = "";
    char regexp_string[MaxRegExpLen] = "";
    char *op[4] = {"eax", "ebx", "ecx", "edx"};
    int i, j, depth, restart;
    printf("\n--- Build XchgREG Gadgets ---\n");
    *xchgREG = (struct Gadget *)malloc(sizeof(struct Gadget));
    if(!*xchgREG)
    {
        fprintf(stderr ,"malloc failed.\n");
        exit(-1);
    }
    rop_chain_list_init(*xchgREG);

    for(i = 0; i < 4; i++)
    {
        for(j = 0; j < 4; j++)
        {
            restart = 0;
            strcpy(regexp_string, "xchg ___, ___");
            strncpy(&regexp_string[5], op[i], 3);
            strncpy(&regexp_string[10], op[j], 3);
            for(depth = 1; depth < arg->depth; depth++)
            {
                memset(gadget_string, 0, MaxGadgetLen);
                temp = tree_search(root, regexp_string, gadget_string, depth, arg);
                if(temp)
                {
                    printf(" O| 0x%08x -> Find XCHG Gadget \"%s\"\n", temp->address, gadget_string);
                    break;
                }
                else if(depth == arg->depth-1)
                {
                    if(arg->verbose)
                    {
                        printf(" X| Can't find gadget \"%s\"\n", regexp_string);
                    }
                    restart = 1;
                    break;
                }
            }
            if(restart)
            {
                continue;
            }
            valid = rop_chain_list_add(*xchgREG, temp->address, gadget_string, 1);
            if(valid == -1)
            {
                temp->vaild = 0;
                j--;
                continue;
            }
        }
    }
    return 0;
}

int rop_build_add_register_gadget(struct Node *root, struct Gadget **addREG, struct Arg *arg)
{
    struct Node *temp;
    char gadget_string[MaxGadgetLen] = "";
    char regexp_string[MaxRegExpLen] = "";
    char *op[4] = {"eax", "ebx", "ecx", "edx"};
    int i, depth, restart;
    int  inc_valid = -1;
    printf("\n--- Build addREG Gadgets ---\n");
    *addREG = (struct Gadget *)malloc(sizeof(struct Gadget));
    if(!*addREG)
    {
        fprintf(stderr ,"malloc failed.\n");
        exit(-1);
    }
    rop_chain_list_init(*addREG);

    for(i = 0; i < 4; i++)
    {
        /* Find inc gadget */
        restart = 0;
        strcpy(regexp_string, "^inc ___$");
        strncpy(&regexp_string[5], op[i], 3);
        for(depth = 1; depth < arg->depth; depth++)
        {
            memset(gadget_string, 0, MaxGadgetLen);
            temp = tree_search(root, regexp_string, gadget_string, depth, arg);
            if(temp)
            {
                printf(" O| 0x%08x -> Find INC Gadget \"%s\"\n", temp->address, gadget_string);
                break;
            }
            else if(depth == arg->depth-1)
            {
                if(arg->verbose)
                {
                    printf(" X| Can't find gadget \"inc %s\"\n", op[i]);
                }
                restart = 1;
                break;
            }
        }
        if(restart == 1)
        {
            continue;
        }
        inc_valid = rop_chain_list_add(*addREG, temp->address, gadget_string, 1);
        if(inc_valid == -1)
        {
            temp->vaild = 0;
            i--;
            continue;
        }
    }
    return 0;
}

int rop_build_interrupt_gadget(struct Node *root, struct Gadget **INT, struct Arg *arg)
{
    struct Node *temp;
    char gadget_string[MaxGadgetLen] = "";
    char regexp_string[MaxRegExpLen] = "";
    printf("\n--- Build interrupt Gadgets ---\n");
    *INT = (struct Gadget *)malloc(sizeof(struct Gadget));
    if(!*INT)
    {
        fprintf(stderr ,"malloc failed.\n");
        exit(-1);
    }
    rop_chain_list_init(*INT);
    memset(gadget_string, 0, MaxGadgetLen);
    strcpy(regexp_string, "int 0x80");
    temp = tree_search(root, regexp_string, gadget_string, 1, arg);
    if(temp)
    {
        printf(" O| 0x%08x -> Find INT Gadget \"%s\"\n", temp->address, gadget_string);
        rop_chain_list_add(*INT, temp->address, gadget_string, 1);
        return 0;
    }
    else
    {
        if(arg->verbose)
        {
            printf(" X| Can't find gadget \"%s\"\n", regexp_string);
        }
        return -1;
    }
}

int rop_gadget_info_update(struct Gadget *gadget)
{
    int i, valid;
    char *instr, *end_instr;
    char string[MaxGadgetLen];
    gadget->padding = 0;
    gadget->gadget_write_no = 0;
    strcpy(gadget->target_write, "nul");
    if(strchr(gadget->string, ';'))
    {
        strcpy(string, gadget->string);
        string[strlen(string) - 1] = '\x00';
        instr = strtok_r(string, ";", &end_instr);
        if(instr != NULL)
        {
            valid = rop_parse_instruction(instr, gadget);
            if(valid == -1)
            {
                return -1;
            }
        }
    }
    while(instr != NULL)
    {
        instr = strtok_r(NULL, ";", &end_instr);
        if(instr != NULL)
        {
            instr++;
            valid = rop_parse_instruction(instr, gadget);
            if(valid == -1)
            {
                return -1;
            }
        }
    }
    for(i = 1; i < gadget->gadget_write_no; i++)
    {
        if(!strcmp(gadget->gadget_write[i], gadget->target_write) &&
        strcmp(gadget->target_write, "nul"))
        {
            printf(" X| Overwrite the target register. Try to find other gadget.\n");
            return -1;
        }
    }
    return 0;
}

int rop_parse_instruction(char *instr, struct Gadget *gadget)
{
    char *operation = NULL;
    char *dst_operand = NULL;
    char *src_operand = NULL;
    char *end_str;
    char *reg;
    char string[MaxGadgetLen];
    long padding;
    strcpy(string, instr);
    operation = string;
    /* parse operand */
    if(strchr(string, ','))
    {
        dst_operand = strtok_r(string, ",", &end_str);
        src_operand = strtok_r(NULL, ",", &end_str) + 1;
    }
    dst_operand = strstr(string, " ");
    if(dst_operand)
    {
        *dst_operand = '\x00';
        dst_operand++;
    }
    else
    {
        return 0;
    }
    if(gadget->gadget_write_no == 0)
    {
        reg = gadget->target_write;
        gadget->gadget_write_no++;
    }
    else
    {
        reg = gadget->gadget_write[gadget->gadget_write_no++];
        if(!strcmp(operation, "pop"))
        {
            gadget->padding += 4;
        }
    }
    memset(reg, 0, 4);
    if(strchr(dst_operand, '['))
    {
        strcpy(reg, "nul");
    }
    else if(strstr(dst_operand, "eax") || strstr(dst_operand, "al"))
    {
        strcpy(reg, "eax");
    }
    else if(strstr(dst_operand, "ebx") || strstr(dst_operand, "bl"))
    {
        strcpy(reg, "ebx");
    }
    else if(strstr(dst_operand, "ecx") || strstr(dst_operand, "cl"))
    {
        strcpy(reg, "ecx");
    }
    else if(strstr(dst_operand, "edx") || strstr(dst_operand, "dl"))
    {
        strcpy(reg, "edx");
    }
    else if(strstr(dst_operand, "esp"))
    {
        strcpy(reg, "esp");
        if(!strcmp(operation, "inc"))
        {
            gadget->padding += 1;
        }
        else if(!strcmp(operation, "add"))
        {
            padding = strtol(src_operand, &end_str, 16);
            gadget->padding += (int)padding;
        }
        else
        {
            printf(" X| Can't use esp gadget. Try to find other gadgets.\n");
            return -1;
        }
    }
    else if(strstr(dst_operand, "ebp"))
    {
        strcpy(reg, "ebp");
    }
    else
    {
        strcpy(reg, "nul");
    }
    return 0;
}

void rop_chain_list_init(struct Gadget *head)
{
    head->next = 0;
    head->prev = 0;
    head->total_target_write_no = 0;
    head->gadget_write_no = 0;
}

int rop_chain_list_add(struct Gadget *head, unsigned int address, char *string, int tail)
{
    int valid;
    struct Gadget *gadget;
    if(strlen(string) > MaxGadgetLen)
    {
        fprintf(stderr ,"Gadget buffer overflow.\n");
        return -1;
    }

    gadget = (struct Gadget *)malloc(sizeof(struct Gadget));
    if(!gadget)
    {
        fprintf(stderr ,"malloc failed.\n");
        exit(-1);
    }
    gadget->address = address;
    gadget->next = NULL;
    strcpy(gadget->string, string);
    strcpy(gadget->target_write, "nul");
    valid = rop_gadget_info_update(gadget);
    if(valid == -1)
    {
        return -1;
    }
    if(head->next)
    {
        if(tail == 1)
        {
            gadget->prev = head->prev;
            head->prev->next = gadget;
            head->prev = gadget;
        }
        else
        {
            gadget->next = head->next;
            head->next->prev = gadget;
            head->next = gadget;
        }
    }
    else
    {
        head->next = gadget;
        head->prev = gadget;
    }
    return 0;
}

int rop_chain_list_traverse(struct Gadget *head, unsigned char **chain)
{
    struct Gadget *temp;
    unsigned char *rechain;
    int padding;
    int chain_len = 0;
    size_t i = 0;
    for(temp = head->next; temp; temp = temp->next)
    {
        i++;
        if(strstr(temp->string, "padding"))
        {
            printf("0x%08x: %s\n", temp->address, temp->string);
            padding = atoi(temp->string + 8);
            chain_len += padding;
            rechain = (unsigned char*)realloc(*chain, chain_len);
            if(!rechain)
            {
                fprintf(stderr ,"realloc failed.\n");
                exit(-1);
            }
            *chain = rechain;
            memset(*chain + chain_len - padding, 0x41, padding);
        }
        else
        {
            printf("0x%08x: %s\n", temp->address, temp->string);
            chain_len += 4 * sizeof(unsigned char);
            rechain = (unsigned char*)realloc(*chain, chain_len);
            if(!rechain)
            {
                fprintf(stderr ,"realloc failed.\n");
                exit(-1);
            }
            *chain = rechain;
            memcpy(*chain + chain_len - 4, &temp->address, 4);
        }
    }
    return chain_len;
}

void rop_chain_list_free(struct Gadget *head)
{
    struct Gadget *temp;
    while(head->next != NULL)
    {
        temp = head->next;
        head->next = head->next->next;
        free(temp);
    }
    free(head);
}
