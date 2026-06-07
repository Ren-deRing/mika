#pragma once

#include <stdint.h>

#define NAME_MAX 255
#define PATH_MAX 4096

typedef int64_t  ssize_t;
typedef uint64_t size_t;
typedef int64_t  off_t;

typedef uint64_t dev_t;     // 장치 식별자
typedef uint64_t ino_t;     // Inode 번호
typedef uint32_t mode_t;    // 파일 속성
typedef uint64_t nlink_t;   // 하드 링크 수
typedef uint32_t uid_t;     // User ID
typedef uint32_t gid_t;     // Group ID
typedef uint64_t blksize_t; // 블록 크기
typedef uint64_t blkcnt_t;  // 블록 개수

typedef int64_t  time_t;

struct timespec {
    time_t tv_sec;          // s
    int64_t tv_nsec;        // ns
};

typedef int32_t pid_t;
typedef int32_t tid_t;