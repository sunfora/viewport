#include <stdbool.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <termios.h>
#include <sys/ioctl.h>

// RESEARCH(ivan): I plan to go straight unicode support
//                 so look for utf8proc library
//                  
//                 also: vim displays big chars like unicode rocket 
//                 as grey '>' when it goes off the screen - nice solution!

#ifndef NO_DEBUG
#define DEBUGGER do { \
    const char *debug_env = getenv("DEBUG"); \
    if (debug_env == NULL || strcmp(debug_env, "skip") != 0) { \
        __asm__("int $3"); \
    } \
} while(0)
#else
#define DEBUGGER
#endif

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define WAIT_FOREVER -1

#define SELECT_DATA  0
#define SELECT_CTRL  1

#define    B(x) ((uintptr_t)(x) <<  0)
#define   KB(x) ((uintptr_t)(x) << 10)
#define   MB(x) ((uintptr_t)(x) << 20)
#define   GB(x) ((uintptr_t)(x) << 30)
#define   TB(x) ((uintptr_t)(x) << 40)

int  main       (int   argc  , char** argv    );
int  entrypoint (void* memory, size_t reserved);

void print_help_message();

// settings and app state
bool  asked_to_print_help         = false;

// bunch of tty nonsense
bool  tty_was_already_in_prefered_mode = false;
struct termios saved_tty_settings = {0};
char* tty_filename                = "/dev/tty";

int   ctrl_fileno  = -1;
int   data_fileno  = -1;

char* watch_filename              = "";
bool  pager_in_a_file_watch_mode  = false;
bool  stdin_equals_tty            = false;
bool  stdin_points_to_tty         = false;

/* 
 * TODO(ivan): fine tune & parametrize this in future via command line arg 
 *             -max-page-size or alike
 */
size_t memory_length              = GB(1); 

void print_help_message() {
  puts(
    "viewport - a pager which handles sreen clearing nice   " "\n"
    "           keeping your relative position in log stable" "\n"
    "                                                       " "\n"
    "usage: ... | viewport [-tty=<tty_path>]                " "\n"
    "       viewport [-tty=<tty_path>] <file_path>          " "\n"
    "                                                       " "\n"
    "arguments:                                             " "\n"
    "       -tty=<tty_path>        - specify tty            " "\n"
  );
}

int gnu_linux_are_two_ttys_the_same(int fd_tty_a, int fd_tty_b, bool *same) {
  // RESEARCH(ivan): it seems to be true that on gnu linux 
  //                 the pair of (st_dev, st_inode)
  //                 
  //                 is a unique identifier of any file-like resource
  //                 and the actual backing
  //
  //                 even though this vvv code below compares tty's
  //                 in the future we might want to decouple it a little bit
  //                 from strict restriction that CTRL device must be tty
  //
  //                 why not sockets or pipes? though we should anyway 
  //                 dispatch between any such thing to disable buffering
  //
  //                 anyway making it work for tty is the priority
  unsigned int dev_a;
  unsigned int dev_b;
  if (ioctl(fd_tty_a, TIOCGDEV, &dev_a) < 0) {
    return -1;
  }
  if (ioctl(fd_tty_b, TIOCGDEV, &dev_b) < 0) {
    return -1;
  }
  
  *same = (dev_a == dev_b);
  return 0;
}

int main(int argc, char** argv) {
  // 
  // here do parse command line arguments 
  //
  int unrecognized = -1;

  for (int i = 1; i < argc; i += 1) {
    bool is_option = (strncmp(argv[i], "-", 1) == 0);

    if (is_option) {
      if (strncmp(argv[i], "-tty=", 5) == 0) {
        tty_filename = argv[i] + 5;
      }
      else if (strcmp(argv[i], "-h") == 0) {
        asked_to_print_help = true;
      } else {
        unrecognized = i;
        continue;
      }
    } else if ((i + 1) != argc) {
      fprintf(
        stderr, 
        "args: filepath must be specified in the end `%s`", argv[i]
      );
      return 1;
    } else {
      watch_filename             = argv[i];
      pager_in_a_file_watch_mode = true;
    }
  }
  
  if (unrecognized != -1) {
    fprintf(stderr, "unrecognized option `%s`\n", argv[unrecognized]);
    return 1;
  }

  if (asked_to_print_help) {
    print_help_message();
    return 0;
  }
  
  // 
  // here do check the tty mess
  //
  ctrl_fileno = open(tty_filename, O_RDONLY | O_NONBLOCK);
  
  if (ctrl_fileno < 0) {
    perror("open(tty_filename, O_RDONLY | O_NONBLOCK)");
    return 1;
  }

  if (!isatty(ctrl_fileno)) {
    perror("isatty(ctrl_fileno)");
    return 1;
  }

  if (isatty(STDIN_FILENO)) {
    stdin_points_to_tty = true;
  } else {
    stdin_points_to_tty = false;
    if (errno != EBADF) {
      errno = 0;
    } else {
      perror("isatty(STDIN_FILENO)");
      return 1;
    }
  }

  if (stdin_points_to_tty) {
    if (gnu_linux_are_two_ttys_the_same(STDIN_FILENO, ctrl_fileno, &stdin_equals_tty) < 0) {
      perror("gnu_linux_are_two_ttys_the_same(STDIN_FILENO, ctrl_fileno, &stdin_equals_tty)");
      return 1;
    }
  }
  
  if (stdin_equals_tty && !pager_in_a_file_watch_mode) {
    fprintf(
      stderr, 
      "viewport: viewport is an interactive pager, it cannot read data" "\n"
      "          and use arrows for navigation when stdin & tty"        "\n"
      "          refer to the same tty"                                 "\n"
      ""                                                                "\n"
      "          either use pager in a file watch mode or use"          "\n"
      "          different tty than stdin"                              "\n"    
    );
    return 1;
  }

  if (pager_in_a_file_watch_mode) {
    fprintf(
      stderr, 
      "The file watch mode with inotify is not implemented yet" "\n"
    );
    return 1;
  }

  data_fileno = STDIN_FILENO;

  //
  // here do fix the tty raw / canonical mess
  // 
  tcflag_t settings_we_dislike = (ICANON | ECHO);

  {
    if (tcgetattr(ctrl_fileno, &saved_tty_settings) == 0) {
      tty_was_already_in_prefered_mode = !( 
          saved_tty_settings.c_lflag & settings_we_dislike
      );
    } else  {
      perror("tcgetattr(ctrl_fileno, &saved_tty_settings)");
      return 1;
    }
  }

  // only save the settings and mess with tty 
  // if tty is not in raw mode already
  if (!tty_was_already_in_prefered_mode) {
    struct termios adequate_input = saved_tty_settings;
    adequate_input.c_lflag &= ~settings_we_dislike;
    if (tcsetattr(ctrl_fileno, TCSAFLUSH, &adequate_input) != 0) {
      perror("tcsetattr(ctrl_fileno, TCSAFLUSH, &adequate_input)");
      return 1;
    }
  }

  //
  // TODO(ivan): register here the callbacks on exit 
  //             and sigint / sigkill / ctrl + z
  //
  //             reset termios settings for the terminal
  //


  //
  // here do reserve the memory
  //
  void* memory = mmap(
      NULL, 
      memory_length,
      PROT_READ | PROT_WRITE, 
      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0
  );
  if (memory == MAP_FAILED) {
    perror(
      "mmap(NULL, memory_length, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)"
    );
    return 1;
  }

  return entrypoint(memory, memory_length);
}

int entrypoint(void *memory, size_t memory_length) {
  struct pollfd fds[] = {
    {
      .fd     = data_fileno,
      .events = POLLIN,
    },
    {
      .fd     = ctrl_fileno,
      .events = POLLIN,
    }
  };
  const size_t nfds = ARRAY_SIZE(fds);

  while (true) {
    const int poll_result = poll(fds, nfds, WAIT_FOREVER);
    if (poll_result < 0) {
      perror("poll(fds, nfds, WAIT_FOREVER)");
      return 1;
    }

    if (fds[SELECT_DATA].revents & POLLERR) {
      printf("DATA POLLERR\n");
      return 1;
    }
    if (fds[SELECT_DATA].revents & POLLHUP) {
      printf("DATA POLLHUP\n");
      return 0;
    }
    if (fds[SELECT_DATA].revents & POLLIN) {
      // just read and ignore for now
      int bytes_read = read(fds[SELECT_DATA].fd, memory, 1024);
      printf("DATA POLLIN: %d bytes\n", bytes_read);
    }

    if (fds[SELECT_CTRL].revents & POLLERR) {
      printf("CTRL POLLERR\n");
      return 1;
    }
    if (fds[SELECT_CTRL].revents & POLLHUP) {
      printf("CTRL POLLHUP\n");
      return 0;
    }
    if (fds[SELECT_CTRL].revents & POLLIN) {
      // just read and ignore for now
      int bytes_read = read(fds[SELECT_CTRL].fd, memory, 1024);
      printf("CTRL POLLIN: %d bytes\n", bytes_read);

      if (bytes_read < 0) {
        printf("CTRL ERROR: %s (errno: %d)\n", strerror(errno), errno);
      }
    }
  }

  return 0;
}
