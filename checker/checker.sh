#!/bin/bash

correct=0
total=0

# afiseaza scorul final
function show_score {
	echo "Total: $total/40"
}

function run_timeout {
    { time -p sh -c "timeout 20 $1" ; } &> time.txt
    ret=$?

    if [ $ret == 124 ]
    then
        echo "W: Programul a durat mai mult de 20 de secunde"
    elif [ $ret != 0 ]
    then
        echo "W: Rularea nu s-a putut executa cu succes"
        cat time.txt | sed '$d' | sed '$d' | sed '$d'
    fi

    rm -rf time.txt
}

# se compara doua fisiere (parametri: fisier1 fisier2)
function compare_files {
    ok=0

    diff -q -w $1 $2
    if [ $? == 0 ]
    then
        correct=$((correct+1))
    else
        echo "W: Exista diferente intre fisierele $1 si $2"
    fi
}

function test1 {
	echo "Se ruleaza testul 1..."
	cp tests/test1/* .
	correct=0
	run_timeout "mpirun --oversubscribe -np 4 ./tema2"
	compare_files client1_file3 out3.txt
	compare_files client2_file1 out1.txt
	compare_files client3_file1 out1.txt
	compare_files client3_file2 out2.txt
	compare_files client3_file3 out3.txt
	if [ $correct == 5 ]
	then
	    total=$((total+10))
	    echo "OK"
	else
		echo "Testul 1 a picat"
	fi
	rm -rf client*_file*
	rm -rf in*txt
	rm -rf out*txt
	echo ""
}

function test2 {
	echo "Se ruleaza testul 2..."
	cp tests/test2/* .
	correct=0
	run_timeout "mpirun --oversubscribe -np 6 ./tema2"
	compare_files client1_file7 out7.txt
	compare_files client2_file6 out6.txt
	compare_files client3_file4 out4.txt
	compare_files client4_file2 out2.txt
	compare_files client5_file1 out1.txt
	compare_files client5_file4 out4.txt
	compare_files client5_file5 out5.txt
	if [ $correct == 7 ]
	then
	    total=$((total+10))
	    echo "OK"
	else
		echo "Testul 2 a picat"
	fi
	rm -rf client*_file*
	rm -rf in*txt
	rm -rf out*txt
	echo ""
}

function test3 {
	echo "Se ruleaza testul 3..."
	cp tests/test3/* .
	correct=0
	run_timeout "mpirun --oversubscribe -np 5 ./tema2"
	compare_files client1_file2 out2.txt
	compare_files client2_file1 out1.txt
	compare_files client3_file1 out1.txt
	compare_files client4_file2 out2.txt
	if [ $correct == 4 ]
	then
	    total=$((total+10))
	    echo "OK"
	else
		echo "Testul 3 a picat"
	fi
	rm -rf client*_file*
	rm -rf in*txt
	rm -rf out*txt
	echo ""
}

function test4 {
	echo "Se ruleaza testul 4..."
	cp tests/test4/* .
	correct=0
	run_timeout "mpirun --oversubscribe -np 7 ./tema2"
	compare_files client3_file1 out1.txt
	compare_files client4_file1 out1.txt
	compare_files client5_file1 out1.txt
	compare_files client6_file1 out1.txt
	if [ $correct == 4 ]
	then
	    total=$((total+10))
	    echo "OK"
	else
		echo "Testul 4 a picat"
	fi
	rm -rf client*_file*
	rm -rf in*txt
	rm -rf out*txt
	echo ""
}

# printeaza informatii despre rulare
#echo "VMCHECKER_TRACE_CLEANUP"
date

export OMPI_ALLOW_RUN_AS_ROOT=1
export OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1

# se compileaza tema
cd ../src
make clean &> /dev/null
make build &> build.txt

if [ ! -f tema2 ]
then
    echo "E: Nu s-a putut compila tema"
    cat build.txt
    show_score
    rm -rf build.txt
    exit
fi

rm -rf build.txt

mv tema2 ../checker
cd ../checker

echo ""
# test1
# test2
# test3
# test4

START_TEST1=$(date +%s)
test1
END_TEST1=$(date +%s)
echo "Test1 took $((END_TEST1 - START_TEST1)) seconds"
echo ""

START_TEST2=$(date +%s)
test2
END_TEST2=$(date +%s)
echo "Test2 took $((END_TEST2 - START_TEST2)) seconds"
echo ""

START_TEST3=$(date +%s)
test3
END_TEST3=$(date +%s)
echo "Test3 took $((END_TEST3 - START_TEST3)) seconds"
echo ""

START_TEST4=$(date +%s)
test4
END_TEST4=$(date +%s)
echo "Test4 took $((END_TEST4 - START_TEST4)) seconds"
echo ""
TOTAL_TIME=$(( (END_TEST1 - START_TEST1) + (END_TEST2 - START_TEST2) + (END_TEST3 - START_TEST3) + (END_TEST4 - START_TEST4) ))
echo "Total time: $TOTAL_TIME seconds"

make clean &> /dev/null

show_score