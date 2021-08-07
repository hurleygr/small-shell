#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>


struct input {
	char *args[512];
	char *input_path;
	char *output_path;
	bool background;
};

// node in a linked list
struct listNode {
	int val;
	struct listNode* prev;
	struct listNode* next;
};

// checks wstatus from waitpid and prints exit value or termination signal
void print_status(int child, int status) {
	if (child == NULL) {
		printf("exit value %d\n", status);
		fflush(stdout);
	}
	else if WIFEXITED(child) {
		printf("exit value %d\n", WEXITSTATUS(child));
		fflush(stdout);
	}
	else if (WIFSIGNALED(child)) {
		printf("terminated by signal %d\n", WTERMSIG(child));
		fflush(stdout);
	}
};

// finds each instance of "$$" in a string and replaces with parent process' pid
void expand(char* string, const char* process_id)
{
	char buffer[1024] = { 0 };
	char* insert_point = &buffer[0];
	const char* tmp = string;
	size_t id_len = strlen(process_id);

	while (1) {
		const char* p = strstr(tmp, "$$");

		if (p == NULL) {
			strcpy(insert_point, tmp);
			break;
		}

		memcpy(insert_point, tmp, p - tmp);
		insert_point += p - tmp;

		memcpy(insert_point, process_id, id_len);
		insert_point += id_len;

		tmp = p + (2 * sizeof(char));
	}
	strcpy(string, buffer);
}
// global variable to keep track of SIGTSTP calls
bool foreground_only = false;

// toggles foreground_only variable when SIGTSTP signal is received, and prints message
void handle_SIGTSTP(int signo) {
	char* message;
	if (foreground_only) {
		message = "Exiting foreground-only mode\n";
		foreground_only = false;
		write(STDOUT_FILENO, message, 30);
		fflush(stdout);
	}
	else {
		message = "Entering foreground-only mode (& is now ignored)\n";
		foreground_only = true;
		write(STDOUT_FILENO, message, 50);
		fflush(stdout);
	}
}

void handle_SIGINT(int signo) {
	char* message = "Terminated by signal 2\n";
	write(STDOUT_FILENO, message, 24);
	fflush(stdout);
	signal(SIGINT, SIG_DFL);

}
int main() {
	int pid = getpid();
	char pid_string[5];
	sprintf(pid_string, "%d", pid);

	int childStatus = NULL;
	int bgChildStatus = NULL;
	int exitStatus = 0;

	// initialize double ended linked list to store background process pid's
	struct listNode* head = malloc(sizeof(struct listNode));
	struct listNode* tail = malloc(sizeof(struct listNode));
	head->next = tail;
	head->val = NULL;
	head->prev = NULL;
	tail->prev = head;
	tail->val = NULL;
	tail->next = NULL;
	

	while (1) {
		sigset_t my_signal_set;
		sigemptyset(&my_signal_set);
		sigaddset(&my_signal_set, SIGTSTP);

		struct sigaction SIGTSTP_action = { 0 };
		SIGTSTP_action.sa_handler = handle_SIGTSTP;
		sigfillset(&SIGTSTP_action.sa_mask);
		SIGTSTP_action.sa_flags = SA_RESTART;
		sigaction(SIGTSTP, &SIGTSTP_action, NULL);

		struct sigaction SIGINT_action = { 0 };
		SIGINT_action.sa_handler = SIG_IGN;
		sigfillset(&SIGINT_action.sa_mask);
		SIGINT_action.sa_flags = SA_RESTART;
		sigaction(SIGINT, &SIGINT_action, NULL);

		// save stdin and stdout in case they are redirected later and need to be restored
		int std_in = dup(STDIN_FILENO);
		int std_out = dup(STDOUT_FILENO);

		int arg_cnt = 0;

		struct input* res = malloc(sizeof(struct input));
		res->background = false;
		
		char* buffer = NULL;
		size_t size = 0;
		ssize_t char_num = 0;
		char* saveptr;

		printf(": ");
		fflush(stdout);

		// get user input stored in buffer
		char_num = getline(&buffer, &size, stdin);

		// check if input is just enter key
		if (strcmp(buffer, "\n") == 0) {
			free(buffer);
			continue;
		}
		// remove trailing new line character
		buffer[char_num - 1] = '\0';
		// replace each $$  in buffer with pid
		expand(buffer, pid_string);
		char* token = strtok_r(buffer, " ", &saveptr);
		// Check if input is a comment
		if (token[0] == '#') {
			continue;
		}
		// tokenize input
		while (token != NULL) {
			if (strcmp(token, "<") == 0) {
				// Next token should have input path
				char* next_token = strtok_r(NULL, " ", &saveptr);
				res->input_path = malloc(sizeof(char) * strlen(next_token));
				strcpy(res->input_path, next_token);

				//Get file descriptor
				int target_input = open(next_token, O_RDONLY, 0777);
				// if file can't be opened print message to that effect
				if (target_input == -1) {
					printf("Cannot open %s for input\n", next_token);
					fflush(stdout);
					childStatus = NULL;
					exitStatus = 1;
					goto end;
				}
				// redirect input
				int input_result = dup2(target_input, STDIN_FILENO);
				close(target_input);
			}
			// same as above for output
			else if (strcmp(token, ">") == 0) {
				char* next_token = strtok_r(NULL, " ", &saveptr);
				res->output_path = malloc(sizeof(char) * strlen(next_token));
				strcpy(res->output_path, next_token);
				int target_output = open(next_token, O_WRONLY | O_CREAT | O_TRUNC, 0777);
				if (target_output == -1) {
					printf("Cannot open %s for output\n", next_token);
					fflush(stdout);
					childStatus = NULL;
					exitStatus = 1;
					goto end;
				}
				int output_result = dup2(target_output, STDOUT_FILENO);
				close(target_output);
			}
			// if & is at end and not in foreground_only mode, set res->background to true
			else if (strcmp(token, "&") == 0) {
				char* next_token = strtok_r(NULL, " ", &saveptr);
				if (next_token == NULL && !foreground_only) {
					res->background = true;
				}
				else {
					res->args[arg_cnt] = next_token;
					arg_cnt += 1;
				}
			}
			else {
				// all other tokens get added to args array
				res->args[arg_cnt] = token;
				arg_cnt += 1;
			}
			token = strtok_r(NULL, " ", &saveptr);
		}
		// null terminate args array for later exec call
		res->args[arg_cnt] = NULL;
		//check if command is one of three built in
		if (strcmp(res->args[0], "cd") == 0) {
			if (res->args[1] == NULL) {
				chdir(getenv("HOME"));
			}
			else {
				chdir(res->args[1]);
			}
		}
		else if (strcmp(res->args[0], "exit") == 0) {
			//kill background processes
			struct listNode* node = head;

			while (node != NULL && node->val != NULL) {
				int child = waitpid(node->val, &bgChildStatus, WNOHANG);
				if (child == 0) {
					kill(child, SIGTERM);
				}
			}
			exit(0);
		}
		else if (strcmp(res->args[0], "status") == 0) {			
			print_status(childStatus, exitStatus);	
		}
		else if (res->background) {
			// ignore SIGSTP signal
			SIGTSTP_action.sa_handler = SIG_IGN;
			sigaction(SIGTSTP, &SIGTSTP_action, NULL);

			pid_t child = fork();
			
			switch (child) {
			case -1:
				perror("fork()\n");
				exit(1);
				break;
			case 0:
				// redirect to dev/null if no other input or output specified
				if (res->output_path == NULL) {
					int devNull = open("/dev/null", O_WRONLY);
					dup2(devNull, STDOUT_FILENO);
				}
				if (res->input_path == NULL) {
					int devNull = open("/dev/null", O_RDONLY);
					dup2(devNull, STDIN_FILENO);
				}
				// execute args
				execvp(res->args[0], res->args);
				perror("execvp");
				exit(2);
				break;
			default:
				printf("background pid is %d\n", child);
				fflush(stdout);
				// add background process to linked list
				struct listNode* new_node = malloc(sizeof(struct listNode));
				new_node->val = child;
				// store in head if empty
				if (head->val == NULL) {
					head->val = child;
				} // store in tail if empty
				else if (tail->val == NULL) {
					tail->val = child;
				} // make new_node the new tail otherwise
				else {
					new_node->prev = tail;
					tail->next = new_node;
					tail = new_node;
					tail->next = NULL;
				}
			}
		}
		else {
			//Foreground
			SIGTSTP_action.sa_handler = SIG_IGN;
			sigaction(SIGTSTP, &SIGTSTP_action, NULL);

			SIGINT_action.sa_handler = handle_SIGINT;
			sigaction(SIGINT, &SIGINT_action, NULL);

			pid_t child = fork();
			switch (child) {
			case -1:
				perror("fork()\n");
				exit(1);
				break;
			case 0:
				execvp(res->args[0], res->args);
				perror("execvp");
				exit(2);
				break;
			default:
				// switch SIGTSTP handler and block its signal
				SIGTSTP_action.sa_handler = handle_SIGTSTP;
				sigaction(SIGTSTP, &SIGTSTP_action, NULL);
				sigprocmask(SIG_BLOCK, &my_signal_set, NULL);

				child = waitpid(child, &childStatus, 0);
				// stop blocking after child terminates
				sigprocmask(SIG_UNBLOCK, &my_signal_set, NULL);
				// restore ignoring SIGINT
				SIGINT_action.sa_handler = SIG_IGN;
				sigaction(SIGINT, &SIGINT_action, NULL);	
			}
		}
		// restore input and output, check for terminated background processes, and free variables
	end:
		dup2(std_in, STDIN_FILENO);
		dup2(std_out, STDOUT_FILENO);

		struct listNode* node = head;

		while (node != NULL && node->val != NULL) {
			int child = waitpid(node->val, &bgChildStatus, WNOHANG);
			// if process has terminated, print message and delete its node from the list
			if (child != 0) {
				printf("background pid %d is done: ", child);
				fflush(stdout);
				print_status(bgChildStatus, exitStatus);
				// if its the head, make next node the head unless its the tail
				if (node->prev == NULL) {
					// if next node is tail and is empty, make head empty
					if (node->next->val == NULL) {
						node->val = NULL;
					}
					// if next is tail and isn't empty, make new head tail, make tail empty
					else if (node->next->next == NULL) {
						head->val = node->next->val;
						node->next->val = NULL;
					}
					// otherwise make node after head new head
					else {
						node->next->prev = NULL;
						head = node->next;
					}
				}// if its the tail, make prev node the tail unless its the head
				else if (node->next == NULL) {
					// if prev is head, make tail empty
					if (node->prev->prev == NULL) {
						node->val = NULL;
					}
					// make previous node new tail
					else {
						node->prev->next = NULL;
						tail = node->prev;
					}
				} // delete normally
				else {
					node->prev->next = node->next;
					node->next->prev = node->prev;
				}
			}
			node = node->next;
		}
		free(buffer);
		free(res);	
	}
}