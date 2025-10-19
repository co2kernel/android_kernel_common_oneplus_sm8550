#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/moduleparam.h>
#include <linux/ctype.h>

#include "../of_private.h"

#define PATCH_TAG "overwrite_configs"

static int parse_numbers(const char *value_str, u8 **out_buf, size_t *out_len)
{
    char *dup, *p, *token_start;
    u8 *buf;
    size_t count = 0, i = 0;
    unsigned long val;
    char *endptr;
    bool in_token = false;

    dup = kstrdup(value_str, GFP_ATOMIC);
    if (!dup)
        return -ENOMEM;

    p = dup;
    while (*p) {
        if (*p != ' ' && *p != '\t' && *p != '\n') {
            if (!in_token) {
                count++;
                in_token = true;
            }
        } else {
            in_token = false;
        }
        p++;
    }

    if (count == 0) {
        kfree(dup);
        return -EINVAL;
    }

    pr_info("parse_numbers: found %zu tokens in '%s'\n", count, value_str);

    buf = kmalloc(count * 4, GFP_ATOMIC);
    if (!buf) {
        kfree(dup);
        return -ENOMEM;
    }

    p = dup;
    token_start = NULL;
    in_token = false;
    
    while (*p && i < count) {
        if (*p != ' ' && *p != '\t' && *p != '\n') {
            if (!in_token) {
                token_start = p;
                in_token = true;
            }
        } else {
            if (in_token) {
                *p = '\0';
                
                pr_info("parse_numbers: parsing token '%s'\n", token_start);
                
                if (strncmp(token_start, "0x", 2) == 0 || strncmp(token_start, "0X", 2) == 0) {
                    val = simple_strtoul(token_start, &endptr, 16);
                } else {
                    val = simple_strtoul(token_start, &endptr, 10);
                }

                if (endptr == token_start || *endptr != '\0' || val > 0xFFFFFFFFUL) {
                    pr_err("parse_numbers: invalid number '%s' (must be <= 0xFFFFFFFF)\n", token_start);
                    kfree(buf);
                    kfree(dup);
                    return -EINVAL;
                }

                buf[i * 4 + 0] = (u8)((val >> 24) & 0xFF);
                buf[i * 4 + 1] = (u8)((val >> 16) & 0xFF);
                buf[i * 4 + 2] = (u8)((val >> 8) & 0xFF);
                buf[i * 4 + 3] = (u8)(val & 0xFF);
                
                pr_info("parse_numbers: parsed value[%zu] = 0x%08lx -> [0x%02x, 0x%02x, 0x%02x, 0x%02x]\n", 
                        i, val, buf[i * 4 + 0], buf[i * 4 + 1], buf[i * 4 + 2], buf[i * 4 + 3]);
                i++;
                in_token = false;
            }
        }
        p++;
    }

    if (in_token && token_start && i < count) {
        pr_info("parse_numbers: parsing final token '%s'\n", token_start);
        
        if (strncmp(token_start, "0x", 2) == 0 || strncmp(token_start, "0X", 2) == 0) {
            val = simple_strtoul(token_start, &endptr, 16);
        } else {
            val = simple_strtoul(token_start, &endptr, 10);
        }

        if (endptr == token_start || *endptr != '\0' || val > 0xFFFFFFFFUL) {
            pr_err("parse_numbers: invalid number '%s' (must be <= 0xFFFFFFFF)\n", token_start);
            kfree(buf);
            kfree(dup);
            return -EINVAL;
        }

        buf[i * 4 + 0] = (u8)((val >> 24) & 0xFF);
        buf[i * 4 + 1] = (u8)((val >> 16) & 0xFF);
        buf[i * 4 + 2] = (u8)((val >> 8) & 0xFF);
        buf[i * 4 + 3] = (u8)(val & 0xFF);
        
        pr_info("parse_numbers: parsed final value[%zu] = 0x%08lx -> [0x%02x, 0x%02x, 0x%02x, 0x%02x]\n", 
                i, val, buf[i * 4 + 0], buf[i * 4 + 1], buf[i * 4 + 2], buf[i * 4 + 3]);
        i++;
    }

    *out_buf = buf;
    *out_len = i * 4;
    kfree(dup);
    
    pr_info("parse_numbers: successfully parsed %zu values (%zu bytes)\n", i, *out_len);
    return 0;
}

static bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_decimal_digit(char c)
{
    return (c >= '0' && c <= '9');
}

static bool is_numeric_value(const char *value)
{
    const char *p = value;
    bool has_non_space = false;

    while (*p && (*p == ' ' || *p == '\t' || *p == '\n'))
        p++;

    while (*p) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n'))
            p++;
        
        if (!*p)
            break;
        
        has_non_space = true;

        if (strncmp(p, "0x", 2) == 0 || strncmp(p, "0X", 2) == 0) {
            p += 2;
            if (!*p || (!is_hex_digit(*p)))
                return false;
            while (*p && is_hex_digit(*p))
                p++;
        } else if (is_decimal_digit(*p)) {
            while (*p && is_decimal_digit(*p))
                p++;
        } else {
            return false;
        }

        if (*p && *p != ' ' && *p != '\t' && *p != '\n')
            return false;
    }
    
    return has_non_space;
}

static int __init remove_dt_node(const char *path)
{
    struct device_node *np, *parent, *child, *prev_child;
    unsigned long flags;
    int ret = 0;

    pr_info("%s: Removing node: '%s'\n", PATCH_TAG, path);

    np = of_find_node_by_path(path);
    if (!np) {
        pr_err("%s: DT node not found: '%s'\n", PATCH_TAG, path);
        return -ENODEV;
    }

    parent = of_get_parent(np);
    if (!parent) {
        pr_err("%s: Cannot remove root node: '%s'\n", PATCH_TAG, path);
        of_node_put(np);
        return -EINVAL;
    }

    raw_spin_lock_irqsave(&devtree_lock, flags);
    
    child = parent->child;
    prev_child = NULL;
    
    while (child) {
        if (child == np) {
            if (prev_child) {
                prev_child->sibling = child->sibling;
            } else {
                parent->child = child->sibling;
            }

            child->parent = NULL;
            child->sibling = NULL;
            
            pr_info("%s: Successfully removed node: '%s'\n", PATCH_TAG, path);
            break;
        }
        prev_child = child;
        child = child->sibling;
    }
    
    raw_spin_unlock_irqrestore(&devtree_lock, flags);
    
    of_node_put(parent);
    of_node_put(np);
    return ret;
}

static int __init remove_dt_property(const char *path, const char *prop_name)
{
    struct device_node *np;
    struct property *prop, *prev_prop;
    unsigned long flags;
    int ret = 0;

    pr_info("%s: Removing property '%s' from node: '%s'\n", PATCH_TAG, prop_name, path);

    np = of_find_node_by_path(path);
    if (!np) {
        pr_err("%s: DT node not found: '%s'\n", PATCH_TAG, path);
        return -ENODEV;
    }

    raw_spin_lock_irqsave(&devtree_lock, flags);
    
    prop = np->properties;
    prev_prop = NULL;
    
    while (prop) {
        if (strcmp(prop->name, prop_name) == 0) {
            if (prev_prop) {
                prev_prop->next = prop->next;
            } else {
                np->properties = prop->next;
            }

            prop->next = NULL;
            
            pr_info("%s: Successfully removed property '%s' from node '%s'\n", PATCH_TAG, prop_name, path);
            ret = 0;
            break;
        }
        prev_prop = prop;
        prop = prop->next;
    }
    
    if (!prop) {
        pr_err("%s: Property '%s' not found in node '%s'\n", PATCH_TAG, prop_name, path);
        ret = -EINVAL;
    }
    
    raw_spin_unlock_irqrestore(&devtree_lock, flags);
    
    of_node_put(np);
    return ret;
}

static int __init create_dt_node(const char *path)
{
    struct device_node *np, *parent;
    char *node_name, *parent_path;
    unsigned long flags;
    int ret = 0;

    pr_info("%s: Creating node: '%s'\n", PATCH_TAG, path);

    node_name = strrchr(path, '/');
    if (!node_name || node_name == path) {
        pr_err("%s: Invalid path format for node creation: '%s'\n", PATCH_TAG, path);
        return -EINVAL;
    }

    parent_path = kstrdup(path, GFP_ATOMIC);
    if (!parent_path) {
        pr_err("%s: Failed to allocate memory for parent path\n", PATCH_TAG);
        return -ENOMEM;
    }

    parent_path[node_name - path] = '\0';
    node_name++;

    parent = of_find_node_by_path(parent_path);
    if (!parent) {
        pr_err("%s: Parent node not found: '%s'\n", PATCH_TAG, parent_path);
        kfree(parent_path);
        return -ENODEV;
    }

    np = of_find_node_by_path(path);
    if (np) {
        pr_info("%s: Node '%s' already exists\n", PATCH_TAG, path);
        of_node_put(np);
        of_node_put(parent);
        kfree(parent_path);
        return 0;
    }

    np = kzalloc(sizeof(*np), GFP_ATOMIC);
    if (!np) {
        pr_err("%s: Failed to allocate memory for new node\n", PATCH_TAG);
        of_node_put(parent);
        kfree(parent_path);
        return -ENOMEM;
    }

    of_node_init(np);

    np->name = kstrdup(node_name, GFP_ATOMIC);
    if (!np->name) {
        pr_err("%s: Failed to allocate memory for node name\n", PATCH_TAG);
        kfree(np);
        of_node_put(parent);
        kfree(parent_path);
        return -ENOMEM;
    }

    np->full_name = kstrdup(path, GFP_ATOMIC);
    if (!np->full_name) {
        pr_err("%s: Failed to allocate memory for full node name\n", PATCH_TAG);
        kfree(np->name);
        kfree(np);
        of_node_put(parent);
        kfree(parent_path);
        return -ENOMEM;
    }

    raw_spin_lock_irqsave(&devtree_lock, flags);
    np->parent = of_node_get(parent);
    np->sibling = parent->child;
    parent->child = np;
    raw_spin_unlock_irqrestore(&devtree_lock, flags);

    pr_info("%s: Successfully created node: '%s'\n", PATCH_TAG, path);

    of_node_put(parent);
    kfree(parent_path);
    return ret;
}

static int parse_hex_bytes(const char *value_str, u8 **out_buf, size_t *out_len)
{
    char *dup, *p, *token_start;
    u8 *buf;
    size_t count, i;
    unsigned long val;
    char *endptr;
    char byte_str[3];

    count = 0, i = 0;

    if (!value_str || strlen(value_str) < 2 || value_str[0] != '[' || value_str[strlen(value_str) - 1] != ']') {
        pr_err("parse_hex_bytes: Invalid format, must be like '[00 FF 12]'\n");
        return -EINVAL;
    }

    dup = kstrdup(value_str + 1, GFP_ATOMIC);
    if (!dup)
        return -ENOMEM;
    dup[strlen(dup) - 1] = '\0';

    p = dup;
    while (*p) {
        if (is_hex_digit(*p)) {
            count++;
            p++; 
            if (is_hex_digit(*p)) p++;
            while (*p && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
        } else if (*p == ' ' || *p == '\t' || *p == '\n') {
            p++;
        } else {
            pr_err("parse_hex_bytes: Invalid character '%c' in hex string '%s'\n", *p, value_str);
            kfree(dup);
            return -EINVAL;
        }
    }

    if (count == 0) {
        pr_info("parse_hex_bytes: No hex bytes found in '%s'\n", value_str);
        *out_buf = NULL;
        *out_len = 0;
        kfree(dup);
        return 0;
    }

    pr_info("parse_hex_bytes: found %zu bytes in '%s'\n", count, value_str);

    buf = kmalloc(count, GFP_ATOMIC);
    if (!buf) {
        kfree(dup);
        return -ENOMEM;
    }

    p = dup;
    i = 0;

    while (*p && i < count) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n')) p++;

        if (!*p) break;

        token_start = p;
        if (!is_hex_digit(token_start[0]) || !is_hex_digit(token_start[1])) {
            pr_err("parse_hex_bytes: Invalid hex byte format '%s' in '%s'\n", token_start, value_str);
            kfree(buf);
            kfree(dup);
            return -EINVAL;
        }
        p += 2;

        strncpy(byte_str, token_start, 2);
        byte_str[2] = '\0';

        val = simple_strtoul(byte_str, &endptr, 16);
        if (endptr == byte_str || *endptr != '\0') {
            pr_err("parse_hex_bytes: invalid hex byte '%s'\n", byte_str);
            kfree(buf);
            kfree(dup);
            return -EINVAL;
        }
        
        buf[i++] = (u8)val;
    }

    *out_buf = buf;
    *out_len = i;
    kfree(dup);

    pr_info("parse_hex_bytes: successfully parsed %zu bytes\n", i);
    return 0;
}


static int __init create_dt_property(const char *path, const char *prop_name, const char *value)
{
    struct device_node *np;
    struct property *prop;
    unsigned long flags;
    u8 *bin_value = NULL;
    size_t bin_len;
    int ret = 0;
    bool is_string_value;
    bool is_hex_byte_value = false;

    pr_info("%s: Creating property '%s' in node '%s' with value '%s'\n", PATCH_TAG, prop_name, path, value ? value : "(null)");

    np = of_find_node_by_path(path);
    if (!np) {
        pr_err("%s: DT node not found: '%s'\n", PATCH_TAG, path);
        return -ENODEV;
    }

    prop = of_find_property(np, prop_name, NULL);
    if (prop) {
        pr_info("%s: Property '%s' already exists in node '%s'\n", PATCH_TAG, prop_name, path);
        of_node_put(np);
        return -EEXIST;
    }

    prop = kzalloc(sizeof(*prop), GFP_ATOMIC);
    if (!prop) {
        pr_err("%s: Failed to allocate memory for new property\n", PATCH_TAG);
        of_node_put(np);
        return -ENOMEM;
    }

    prop->name = kstrdup(prop_name, GFP_ATOMIC);
    if (!prop->name) {
        pr_err("%s: Failed to allocate memory for property name\n", PATCH_TAG);
        kfree(prop);
        of_node_put(np);
        return -ENOMEM;
    }

    if (!value || value[0] == '\0') {
        bin_value = kzalloc(1, GFP_ATOMIC);
        if (!bin_value) {
            pr_err("%s: Failed to allocate memory for [00] value\n", PATCH_TAG);
            kfree(prop->name);
            kfree(prop);
            of_node_put(np);
            return -ENOMEM;
        }
        bin_value[0] = 0x00;
        prop->length = 1;
        prop->value = bin_value;
        pr_info("%s: Created property '%s' with default [00] value due to empty input\n", PATCH_TAG, prop_name);
    } else {
        if (value[0] == '[' && value[strlen(value) - 1] == ']') {
            is_hex_byte_value = true;
            ret = parse_hex_bytes(value, &bin_value, &bin_len);
            if (ret != 0) {
                pr_err("%s: Failed to parse hex byte value '%s': %d\n", PATCH_TAG, value, ret);
                kfree(prop->name);
                kfree(prop);
                of_node_put(np);
                return ret;
            }
            prop->length = bin_len;
            prop->value = bin_value;
            pr_info("%s: Created hex byte property '%s' with length %zu\n", PATCH_TAG, prop_name, bin_len);
        } else {
            is_string_value = !is_numeric_value(value);

            if (is_string_value) {
                size_t str_len = strlen(value);
                prop->length = str_len + 1;
                prop->value = kzalloc(prop->length, GFP_ATOMIC);
                if (!prop->value) {
                    pr_err("%s: Failed to allocate memory for string value\n", PATCH_TAG);
                    kfree(prop->name);
                    kfree(prop);
                    of_node_put(np);
                    return -ENOMEM;
                }
                memcpy(prop->value, value, str_len);
                ((char *)prop->value)[str_len] = '\0';
                pr_info("%s: Created string property '%s' with value '%s' (len=%d)\n",
                        PATCH_TAG, prop_name, value, prop->length);
            } else {
                ret = parse_numbers(value, &bin_value, &bin_len);
                if (ret != 0) {
                    pr_err("%s: Failed to parse numeric value '%s': %d\n", PATCH_TAG, value, ret);
                    kfree(prop->name);
                    kfree(prop);
                    of_node_put(np);
                    return ret;
                }

                prop->length = bin_len;
                prop->value = bin_value;
                pr_info("%s: Created numeric property '%s' with length %zu\n", PATCH_TAG, prop_name, bin_len);
            }
        }
    }

    raw_spin_lock_irqsave(&devtree_lock, flags);
    prop->next = np->properties;
    np->properties = prop;
    raw_spin_unlock_irqrestore(&devtree_lock, flags);

    of_node_put(np);
    return ret;
}

static int __init patch_device_tree(const char *input)
{
    struct device_node *np;
    struct property *prop;
    char *dup, *path, *prop_name, *value, *space_pos;
    u8 *bin_value = NULL;
    void *old_value = NULL;
    size_t bin_len;
    int ret;
    bool is_string_value = false;
    bool is_hex_byte_value = false;
    char operation;
    char *op_input;
    size_t final_len;

    if (!input || strlen(input) == 0) {
        pr_err("%s: Invalid input\n", PATCH_TAG);
        return -EINVAL;
    }

    pr_info("%s: Input string: '%s'\n", PATCH_TAG, input);

    dup = kstrdup(input, GFP_ATOMIC);
    if (!dup) {
        pr_err("%s: Failed to duplicate input string\n", PATCH_TAG);
        return -ENOMEM;
    }

    operation = dup[0];
    if (operation == 'r' || operation == 'd' || operation == 'c' || operation == 'a') {
        op_input = dup + 1;
        while (*op_input && (*op_input == ' ' || *op_input == '\t'))
            op_input++;
        
        if (operation == 'r') {
            ret = remove_dt_node(op_input);
            kfree(dup);
            return ret;
        } else if (operation == 'd') {
            char *last_slash = strrchr(op_input, '/');
            if (!last_slash || last_slash == op_input) {
                pr_err("%s: Invalid format for remove property operation: no valid path\n", PATCH_TAG);
                kfree(dup);
                return -EINVAL;
            }
            
            *last_slash = '\0';
            path = op_input;
            prop_name = last_slash + 1;
            
            while (*prop_name && (*prop_name == ' ' || *prop_name == '\t'))
                prop_name++;
            
            pr_info("%s: Parsed path: '%s'\n", PATCH_TAG, path);
            pr_info("%s: Property name: '%s'\n", PATCH_TAG, prop_name);
            
            ret = remove_dt_property(path, prop_name);
            kfree(dup);
            return ret;
        } else if (operation == 'c') {
            ret = create_dt_node(op_input);
            kfree(dup);
            return ret;
        } else if (operation == 'a') {
            space_pos = strchr(op_input, ' ');
            if (!space_pos) {
                pr_err("%s: Invalid format for create property operation: no value specified\n", PATCH_TAG);
                kfree(dup);
                return -EINVAL;
            }
            
            *space_pos = '\0';
            path = op_input;
            value = space_pos + 1;
            
            while (*value && (*value == ' ' || *value == '\t' || *value == '\n'))
                value++;
            
            if (*value == '\0') {
                pr_err("%s: Empty value for create property operation\n", PATCH_TAG);
                kfree(dup);
                return -EINVAL;
            }
            
            prop_name = strrchr(path, '/');
            if (!prop_name || prop_name == path) {
                pr_err("%s: Invalid path format for create property: '%s'\n", PATCH_TAG, path);
                kfree(dup);
                return -EINVAL;
            }
            
            *prop_name = '\0';
            prop_name++;
            
            pr_info("%s: Parsed path: '%s'\n", PATCH_TAG, path);
            pr_info("%s: Property name: '%s'\n", PATCH_TAG, prop_name);
            pr_info("%s: Property value: '%s'\n", PATCH_TAG, value);
            
            ret = create_dt_property(path, prop_name, value);
            kfree(dup);
            return ret;
        }
    }

    space_pos = strchr(dup, ' ');
    if (!space_pos) {
        pr_err("%s: Invalid input format, no space found\n", PATCH_TAG);
        kfree(dup);
        return -EINVAL;
    }

    *space_pos = '\0';
    path = dup;
    value = space_pos + 1;

    while (*value && (*value == ' ' || *value == '\t' || *value == '\n'))
        value++;
    
    pr_info("%s: Parsed path: '%s'\n", PATCH_TAG, path);
    pr_info("%s: Parsed value: '%s'\n", PATCH_TAG, value);

    if (*value == '\0') {
        pr_err("%s: Empty value after path\n", PATCH_TAG);
        kfree(dup);
        return -EINVAL;
    }

    prop_name = strrchr(path, '/');
    if (!prop_name || prop_name == path) {
        pr_err("%s: Invalid path format: '%s'\n", PATCH_TAG, path);
        kfree(dup);
        return -EINVAL;
    }

    *prop_name = '\0';
    prop_name++;
    
    pr_info("%s: Node path: '%s'\n", PATCH_TAG, path);
    pr_info("%s: Property name: '%s'\n", PATCH_TAG, prop_name);

    np = of_find_node_by_path(path);
    if (!np) {
        pr_err("%s: DT node not found: '%s'\n", PATCH_TAG, path);
        kfree(dup);
        return -ENODEV;
    }

    prop = of_find_property(np, prop_name, NULL);
    if (!prop) {
        pr_err("%s: Property '%s' not found in node '%s'\n", PATCH_TAG, prop_name, path);
        of_node_put(np);
        kfree(dup);
        return -EINVAL;
    }
    
    pr_info("%s: Found property '%s', current length: %d\n", PATCH_TAG, prop_name, prop->length);

    if (value[0] == '[' && value[strlen(value) - 1] == ']') {
        is_hex_byte_value = true;
        ret = parse_hex_bytes(value, &bin_value, &bin_len);
        if (ret != 0) {
            pr_err("%s: Failed to parse hex byte value '%s': %d\n", PATCH_TAG, value, ret);
            of_node_put(np);
            kfree(dup);
            return ret;
        }

        pr_info("%s: Parsed %zu bytes from hex byte string\n", PATCH_TAG, bin_len);

        old_value = prop->value;
        final_len = bin_len;

        prop->value = kzalloc(final_len, GFP_ATOMIC);
        if (!prop->value) {
            pr_err("%s: Failed to allocate memory for hex byte value\n", PATCH_TAG);
            prop->value = old_value;
            kfree(bin_value);
            of_node_put(np);
            kfree(dup);
            return -ENOMEM;
        }
        memcpy(prop->value, bin_value, bin_len);
        prop->length = final_len;

        pr_info("%s: Patched %s/%s to hex bytes (len=%zu)\n", PATCH_TAG, path, prop_name, final_len);
        kfree(bin_value);

    } else {
        is_string_value = !is_numeric_value(value);
        
        if (is_string_value) {
            size_t str_len = strlen(value);
            size_t final_len = str_len + 1;
            
            pr_info("%s: Treating value as string (len=%zu)\n", PATCH_TAG, str_len);
            old_value = prop->value;
            
            prop->value = kzalloc(final_len, GFP_ATOMIC);
            if (!prop->value) {
                pr_err("%s: Failed to allocate memory for string value\n", PATCH_TAG);
                prop->value = old_value;
                of_node_put(np);
                kfree(dup);
                return -ENOMEM;
            }

            memcpy(prop->value, value, str_len);
            prop->length = final_len;
            
            pr_info("%s: Patched %s/%s to string '%s' (len=%zu)\n", PATCH_TAG, path, prop_name, value, final_len);
        } else {
            ret = parse_numbers(value, &bin_value, &bin_len);
            if (ret != 0) {
                pr_err("%s: Failed to parse numeric value '%s': %d\n", PATCH_TAG, value, ret);
                of_node_put(np);
                kfree(dup);
                return ret;
            }

            pr_info("%s: Parsed %zu bytes from numeric value string\n", PATCH_TAG, bin_len);

            old_value = prop->value;

            final_len = max(bin_len, (size_t)prop->length);
            
            prop->value = kzalloc(final_len, GFP_ATOMIC);
            if (!prop->value) {
                pr_err("%s: Failed to allocate memory for numeric value\n", PATCH_TAG);
                prop->value = old_value;
                kfree(bin_value);
                of_node_put(np);
                kfree(dup);
                return -ENOMEM;
            }

            memcpy(prop->value, bin_value, bin_len);
            prop->length = final_len;

            pr_info("%s: Updated property length from %d to %zu bytes\n", PATCH_TAG, 
                     prop->length, final_len);

            if (bin_len > 0) {
                size_t hex_str_size = bin_len * 5 + 1;
                char *hex_str = kmalloc(hex_str_size, GFP_ATOMIC);
                if (hex_str) {
                    size_t j, pos = 0;
                    hex_str[0] = '\0';
                    for (j = 0; j < bin_len; j++) {
                        int written = snprintf(hex_str + pos, hex_str_size - pos, "0x%02x", bin_value[j]);
                        if (written > 0 && pos + written < hex_str_size) {
                            pos += written;
                        }
                        if (j < bin_len - 1 && pos < hex_str_size - 1) {
                            hex_str[pos++] = ' ';
                            hex_str[pos] = '\0';
                        }
                    }
                    pr_info("%s: Patched %s/%s to %s (len=%zu)\n", PATCH_TAG, path, prop_name, hex_str, bin_len);
                    kfree(hex_str);
                } else {
                    pr_info("%s: Patched %s/%s to binary data (%zu bytes)\n", PATCH_TAG, path, prop_name, bin_len);
                }
            }

            kfree(bin_value);
        }
    }

    if (prop->length > 0) {
        if (is_string_value) {
            pr_info("%s: Verification - string value: '%s', length: %d\n", PATCH_TAG, (char *)prop->value, prop->length);
        } else if (is_hex_byte_value) {
            if (prop->value && prop->length > 0) {
                size_t hex_str_size = prop->length * 3 + 1;
                char *hex_str = kmalloc(hex_str_size, GFP_ATOMIC);
                if (hex_str) {
                    size_t j, pos = 0;
                    hex_str[0] = '[';
                    pos = 1;
                    for (j = 0; j < prop->length; j++) {
                        int written = snprintf(hex_str + pos, hex_str_size - pos, "%02x ", ((u8 *)prop->value)[j]);
                        if (written > 0 && pos + written < hex_str_size) {
                            pos += written;
                        }
                    }
                    if (pos > 1) hex_str[pos-1] = ']';
                    else hex_str[pos++] = ']';
                    hex_str[pos] = '\0';
                    pr_info("%s: Verification - hex byte value: '%s', length: %d\n", PATCH_TAG, hex_str, prop->length);
                    kfree(hex_str);
                } else {
                     pr_info("%s: Verification - hex byte value (binary), length: %d\n", PATCH_TAG, prop->length);
                }
            }
        }
        else {
            u8 *val = (u8 *)prop->value;
            pr_info("%s: Verification - first byte: 0x%02x, length: %d\n", PATCH_TAG, val[0], prop->length);
        }
    }

    of_node_put(np);
    kfree(dup);
    return 0;
}

static char *get_property_from_cmdline(const char *input)
{
    char *cmdline = saved_command_line;
    char *prop_buf = NULL;
    char *prop_start, *prop_end;
    char *prop_prefix;
    int len;

    pr_info("Kernel cmdline: %s\n", cmdline);

    prop_prefix = kmalloc(strlen(input) + 2, GFP_ATOMIC);
    if (!prop_prefix) {
        pr_err("Failed to allocate memory for property prefix\n");
        return NULL;
    }
    snprintf(prop_prefix, strlen(input) + 2, "%s=", input);

    prop_start = strstr(cmdline, prop_prefix);
    if (prop_start) {
        prop_start += strlen(prop_prefix);
        prop_end = strchr(prop_start, ' ');
        len = prop_end ? (prop_end - prop_start) : strlen(prop_start);

        prop_buf = kmalloc(len + 1, GFP_ATOMIC);
        if (prop_buf) {
            strncpy(prop_buf, prop_start, len);
            prop_buf[len] = '\0';
            pr_info("Found property %s: %s\n", input, prop_buf);
        } else {
            pr_err("Failed to allocate memory for property value\n");
        }
    } else {
        pr_err("Property %s not found in cmdline\n", input);
    }

    kfree(prop_prefix);
    return prop_buf ? prop_buf : NULL;
}

static int __init overwrite_config_init(void)
{
    char *device_name = get_property_from_cmdline("oplusboot.prjname");
    
    const struct overwrite_config_group *common_group = NULL;
    const struct overwrite_config_group *device_group = NULL;
    const struct overwrite_config_group *group;
    int i, j, k;

    for (i = 0; i < overwrite_config_group_count; i++) {
        group = &overwrite_config_groups[i];
        
        if (strcmp(group->prefix, "common") == 0) {
            common_group = group;
        } else if (device_name && strcmp(group->prefix, device_name) == 0) {
            device_group = group;
        }
    }

    if (common_group) {
        pr_info("Applying common configs...\n");
        for (j = 0; j < common_group->count; j++) {
            patch_device_tree(common_group->values[j]);
            pr_info("  Applied common patch: %s\n", common_group->values[j]);
        }
    } else {
        pr_info("No common configs found.\n");
    }

    if (device_group) {
        pr_info("Applying device-specific configs for %s...\n", device_name);
        for (k = 0; k < device_group->count; k++) {
            patch_device_tree(device_group->values[k]);
            pr_info("  Applied device patch: %s\n", device_group->values[k]);
        }
    } else {
        pr_info("No device-specific configs found for %s.\n", device_name ? device_name : "none");
    }

    kfree(device_name);
    return 0;
}

early_initcall(overwrite_config_init);