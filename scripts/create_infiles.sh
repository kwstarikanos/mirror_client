#!/bin/bash

generate_random_string() {
    string="";
    pattern="abcdefghijklwricoalc234019495023";
	patternLength=`expr length ${pattern}`;
    stringLength=`expr $RANDOM % 8 + 1`
    for ((sl=0; sl <= stringLength; sl++)) do
        ch=`expr $RANDOM % ${patternLength} + 1`;
        string+=`expr substr ${pattern} ${ch} 1`;
    done
}

validate_args() {
    if [[  $# -ge 4 ]]; then
    shift
        for str
            do
            if (! [[ ${1} =~ ^[0-9]+$ ]] ); then
                (>&2 echo "Error: Invalid arguments!")
                exit 1
            fi
            shift
            done
    else
        (>&2 echo "Error: Too few arguments!")
        exit 1
    fi
}

#MAIN

echo "Number of arguments: [$#]"

validate_args $@

path=$1;

for ((i = 0; i < $3; i++))
do
    generate_random_string

    lv=`expr ${i} % $4`

    if [[ ${lv} > 0 ]]; then
        path="${path}/${string}"
    else
        path="$1/${string}"
    fi

    echo "mkdir -p ${path}"

    mkdir -p ${path}

done
