#pragma once

#define O_RDONLY    0x0000    /* 읽기 전용 */
#define O_WRONLY    0x0001    /* 쓰기 전용 */
#define O_RDWR      0x0002    /* 읽기/쓰기 */
#define O_ACCMODE   0x0003    /* 접근 모드 마스크 */

#define O_CREAT     0x0040    /* 파일이 없으면 생성 */
#define O_EXCL      0x0080    /* 파일이 있으면 에러 */
#define O_TRUNC     0x0200    /* 기존 내용 삭제 */
#define O_APPEND    0x0400    /* 이어 쓰기 */
#define O_NONBLOCK  0x0800    /* 논블로킹 I/O */
#define O_CLOEXEC   0x80000   /* 실행 시 닫기 */
#define O_DIRECTORY 0x010000  /* 디렉토리가 아니면 에러 */

#define SEEK_SET    0         /* 커서를 offset 위치로 설정 */
#define SEEK_CUR    1         /* 현재 위치에서 offset 만큼 이동 */
#define SEEK_END    2         /* 파일 끝에서 offset 만큼 이동 */

#define AT_EMPTY_PATH 0x1000