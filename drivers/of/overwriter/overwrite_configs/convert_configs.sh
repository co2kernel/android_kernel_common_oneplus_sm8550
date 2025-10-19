#!/bin/bash
srctree=$(pwd)

temp=""
temp+="struct overwrite_config_group {\n    const char *prefix;\n    const char *const *values;\n    int count;\n};\n\n"

declare -A config_map
declare -A config_counts

function process_file() {
    local prefix=$1
    local file=$2
    while IFS= read -r line; do
        line=$(echo "$line" | tr -d '\r')
        if [ -n "$line" ]; then
            config_map[$prefix]+="\"$line\","
            ((config_counts[$prefix]++))
        fi
    done < <(tr -d '\r' < "$file")
}

common_dir="$srctree/drivers/of/overwriter/overwrite_configs/common"
if [ -d "$common_dir" ]; then
    for file in "$common_dir"/*.conf; do
        if [ -f "$file" ]; then
            process_file "common" "$file"
        fi
    done
fi

config_base_dir="$srctree/drivers/of/overwriter/overwrite_configs"
for model_dir in "$config_base_dir"/[0-9]*; do
    if [ -d "$model_dir" ]; then
        model_prefix=$(basename "$model_dir")
        for file in "$model_dir"/*.conf; do
            if [ -f "$file" ]; then
                process_file "$model_prefix" "$file"
            fi
        done
    fi
done

for prefix in "${!config_map[@]}"; do
    values_str="${config_map[$prefix]}"

    c_var_name="${prefix}"
    if [[ "$prefix" =~ ^[0-9] ]]; then
        c_var_name="model_${prefix}"
    fi

    temp+="static const char *const ${c_var_name}_values[] = {\n"

    if [ -n "$values_str" ]; then
        temp+="    ${values_str%,}"
    fi
    temp+="};"
    temp+="\n\n"
done

group_count=0

temp+="\nconst struct overwrite_config_group overwrite_config_groups[] = {\n"

for prefix in "${!config_map[@]}"; do
    values_count="${config_counts[$prefix]:-0}"

    c_var_name="${prefix}"
    if [[ "$prefix" =~ ^[0-9] ]]; then
        c_var_name="model_${prefix}"
    fi

    temp+="    { .prefix = \"$prefix\", .values = ${c_var_name}_values, .count = $values_count },\n"
    ((group_count++))
done

temp+="};\n\nconst int overwrite_config_group_count = $group_count;"

tmp=$(mktemp)
printf '%b' "$temp" > "$tmp"

sed -i '/#define PATCH_TAG "overwrite_configs"/r '"$tmp" \
  "$srctree/drivers/of/overwriter/overwrite_config_loader.c"

rm -f "$tmp"
