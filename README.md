# BIL461 – Operating Systems  
## HW1 – Database Management Service

This project implements a multi-process database service in C that simulates
an operating-system style data-processing pipeline using `fork`, `mmap`,
`pipe`, and `execvp`.

### Overview
The program runs in four stages:

1️⃣ **Manager**
- Creates a shared memory mapping using `mmap`
- Spawns worker processes and distributes file regions

2️⃣ **Extractors**
- Read assigned file regions in parallel (without file I/O functions)
- Search for the given keyword
- Write matching records to a shared pipe

3️⃣ **Sorter**
- Reads data from the pipe
- Uses Linux `sort` to sort records by the **5th column (grade)**
- Writes sorted results to the output file

4️⃣ **Reporter**
- Reads the output file
- Uses `wc` to print the number of records

### Technologies & Concepts
- C programming
- Process creation → `fork`
- Shared memory → `mmap`
- IPC → `pipe`
- External programs → `execvp` (`sort`, `wc`)
- No `fread` / `fgets` file APIs (per assignment rules)


### Build
```bash
make
# produces ./database_service
```

### Run
```bash
./database_service <input_file> <output_file> <num_of_workers> <keyword>
```

Example:
```bash
./database_service examples/database_records.txt result.txt 4 "Algorithms"
```

### Notes
- Sorting is performed using `sort` by grade (5th column)
- Reporter prints record count using `wc`
- All system calls are validated and errors are reported via `perror`
