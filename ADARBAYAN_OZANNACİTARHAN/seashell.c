#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>            //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
const char * sysname = "seashell";
const char* findPath(char *cmd);
const char* findLine(int count, char *fname);
int search_list(char *fname, char *str);
int flag = 0;

enum return_codes {
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};
struct command_t {
	char *name;
	bool background;
	bool auto_complete;
	int arg_count;
	char **args;
	char *redirects[3]; // in/out redirection
	struct command_t *next; // for piping
};
/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t * command)
{
	int i=0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background?"yes":"no");
	printf("\tNeeds Auto-complete: %s\n", command->auto_complete?"yes":"no");
	printf("\tRedirects:\n");
	for (i=0;i<3;i++)
		printf("\t\t%d: %s\n", i, command->redirects[i]?command->redirects[i]:"N/A");
	printf("\tArguments (%d):\n", command->arg_count);
	for (i=0;i<command->arg_count;++i)
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	if (command->next)
	{
		printf("\tPiped to:\n");
		print_command(command->next);
	}


}
/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command)
{
	if (command->arg_count)
	{
		for (int i=0; i<command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}
	for (int i=0;i<3;++i)
		if (command->redirects[i])
			free(command->redirects[i]);
	if (command->next)
	{
		free_command(command->next);
		command->next=NULL;
	}
	free(command->name);
	free(command);
	return 0;
}
/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt()
{
	char cwd[1024], hostname[1024];
    gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}
/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command)
{
	const char *splitters=" \t"; // split at whitespace
	int index, len;
	len=strlen(buf);
	while (len>0 && strchr(splitters, buf[0])!=NULL) // trim left whitespace
	{
		buf++;
		len--;
	}
	while (len>0 && strchr(splitters, buf[len-1])!=NULL)
		buf[--len]=0; // trim right whitespace

	if (len>0 && buf[len-1]=='?') // auto-complete
		command->auto_complete=true;
	if (len>0 && buf[len-1]=='&') // background
		command->background=true;

	char *pch = strtok(buf, splitters);
	command->name=(char *)malloc(strlen(pch)+1);
	if (pch==NULL)
		command->name[0]=0;
	else
		strcpy(command->name, pch);

	command->args=(char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index=0;
	char temp_buf[1024], *arg;
	while (1)
	{
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch) break;
		arg=temp_buf;
		strcpy(arg, pch);
		len=strlen(arg);

		if (len==0) continue; // empty arg, go for next
		while (len>0 && strchr(splitters, arg[0])!=NULL) // trim left whitespace
		{
			arg++;
			len--;
		}
		while (len>0 && strchr(splitters, arg[len-1])!=NULL) arg[--len]=0; // trim right whitespace
		if (len==0) continue; // empty arg, go for next

		// piping to another command
		if (strcmp(arg, "|")==0)
		{
			struct command_t *c=malloc(sizeof(struct command_t));
			int l=strlen(pch);
			pch[l]=splitters[0]; // restore strtok termination
			index=1;
			while (pch[index]==' ' || pch[index]=='\t') index++; // skip whitespaces

			parse_command(pch+index, c);
			pch[l]=0; // put back strtok termination
			command->next=c;
			continue;
		}

		// background process
		if (strcmp(arg, "&")==0)
			continue; // handled before

		// handle input redirection
		redirect_index=-1;
		if (arg[0]=='<')
			redirect_index=0;
		if (arg[0]=='>')
		{
			if (len>1 && arg[1]=='>')
			{
				redirect_index=2;
				arg++;
				len--;
			}
			else redirect_index=1;
		}
		if (redirect_index != -1)
		{
			command->redirects[redirect_index]=malloc(len);
			strcpy(command->redirects[redirect_index], arg+1);
			continue;
		}

		// normal arguments
		if (len>2 && ((arg[0]=='"' && arg[len-1]=='"')
			|| (arg[0]=='\'' && arg[len-1]=='\''))) // quote wrapped arg
		{
			arg[--len]=0;
			arg++;
		}
		command->args=(char **)realloc(command->args, sizeof(char *)*(arg_index+1));
		command->args[arg_index]=(char *)malloc(len+1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count=arg_index;
	return 0;
}
void prompt_backspace()
{
	putchar(8); // go back 1
	putchar(' '); // write empty over
	putchar(8); // go back 1 again
}
/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command)
{
	int index=0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

    // tcgetattr gets the parameters of the current terminal
    // STDIN_FILENO will tell tcgetattr that it should write the settings
    // of stdin to oldt
    static struct termios backup_termios, new_termios;
    tcgetattr(STDIN_FILENO, &backup_termios);
    new_termios = backup_termios;
    // ICANON normally takes care that one line at a time will be processed
    // that means it will return if it sees a "\n" or an EOF or an EOL
    new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
    // Those new settings will be set to STDIN
    // TCSANOW tells tcsetattr to change attributes immediately.
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);


    //FIXME: backspace is applied before printing chars
	show_prompt();
	int multicode_state=0;
	buf[0]=0;
  	while (1)
  	{
		c=getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		if (c==9) // handle tab
		{
			buf[index++]='?'; // autocomplete
			break;
		}

		if (c==127) // handle backspace
		{
			if (index>0)
			{
				prompt_backspace();
				index--;
			}
			continue;
		}
		if (c==27 && multicode_state==0) // handle multi-code keys
		{
			multicode_state=1;
			continue;
		}
		if (c==91 && multicode_state==1)
		{
			multicode_state=2;
			continue;
		}
		if (c==65 && multicode_state==2) // up arrow
		{
			int i;
			while (index>0)
			{
				prompt_backspace();
				index--;
			}
			for (i=0;oldbuf[i];++i)
			{
				putchar(oldbuf[i]);
				buf[i]=oldbuf[i];
			}
			index=i;
			continue;
		}
		else
			multicode_state=0;

		putchar(c); // echo the character
		buf[index++]=c;
		if (index>=sizeof(buf)-1) break;
		if (c=='\n') // enter key
			break;
		if (c==4) // Ctrl+D
			return EXIT;
  	}
  	if (index>0 && buf[index-1]=='\n') // trim newline from the end
  		index--;
  	buf[index++]=0; // null terminate string

  	strcpy(oldbuf, buf);

  	parse_command(buf, command);

  	// print_command(command); // DEBUG: uncomment for debugging

    // restore the old settings
    tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
  	return SUCCESS;
}
int process_command(struct command_t *command);
int main()
{
	while (1)
	{
		struct command_t *command=malloc(sizeof(struct command_t));
		memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

		int code;
		code = prompt(command);
		if (code==EXIT) break;

		code = process_command(command);
		if (code==EXIT) break;

		free_command(command);
	}

	printf("\n");
	return 0;
}

int process_command(struct command_t *command)
{
	int r;
	if (strcmp(command->name, "")==0) return SUCCESS;

	if (strcmp(command->name, "exit")==0)
		return EXIT;

	if (strcmp(command->name, "cd")==0)
	{
		if (command->arg_count > 0)
		{
			r=chdir(command->args[0]);
			if (r==-1)
				printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
			return SUCCESS;
		}
	}
	

	pid_t pid=fork();
	if (pid==0) // child
	{
		/// This shows how to do exec with environ (but is not available on MacOs)
	    	// extern char** environ; // environment variables
		// execvpe(command->name, command->args, environ); // exec+args+path+environ

		/// This shows how to do exec with auto-path resolve
		// add a NULL argument to the end of args, and the name to the beginning
		// as required by exec

		// increase args size by 2
		command->args=(char **)realloc(
			command->args, sizeof(char *)*(command->arg_count+=2));

		// shift everything forward by 1
		for (int i=command->arg_count-2;i>0;--i)
			command->args[i]=command->args[i-1];

		// set args[0] as a copy of name
		command->args[0]=strdup(command->name);
		// set args[arg_count-1] (last) to NULL
		command->args[command->arg_count-1]=NULL;

		/// TODO: do your own exec with path resolving using execv()

        	if (command->name[0] == '/' || command->name[0] == '.') {
            		execv(command->name, command->args);
        	}
		else if (strcmp(command->args[0],"rps") == 0){
			time_t seconds = time(NULL);
			int choose = seconds % 3;
			int choose_2;
			if (strcmp(command->args[1], "rock") == 0){
				choose_2 = 0;
			}
			if (strcmp(command->args[1], "paper") == 0){
				choose_2 = 1;
			}
			if (strcmp(command->args[1], "scissors") == 0){
				choose_2 = 2;
			}
			if (choose_2 - choose == 0){
				printf("It is a tie!\n");
			}
			else if (choose_2 - choose == -1 || choose_2 - choose == 2){
				printf("You lose. Better luck next time.\n");
			}
			else{
				printf("You win.\n");
			}
		}
		else if (strcmp(command->args[0],"kdiff") == 0){
			int count = 0;
			if (strcmp(command->args[1], "-b") == 0){
				char *d1;
				d1=(char *)malloc(100*sizeof(char));
				strcat(d1,"cmp -l ");
				strcat(d1,command->args[2]);
				strcat(d1," ");
				strcat(d1,command->args[3]);
				strcat(d1," > bytes.txt");
				system(d1);
				char const* const fileName = "bytes.txt"; 
    				FILE* file = fopen(fileName, "r"); 
    				char line[256];
				int diff = 0;
				while (fgets(line, sizeof(line), file)) {
					diff++;
				}
				if(diff == 0){
					printf("The two files are identical\n");
				}
				else if(diff == 1){
					printf("The two files are different in 1 byte\n");
				}
				else{
					printf("The two files are different in %d bytes\n", diff);
				}
				system("rm bytes.txt");
				
				
			}
			else{ 
				char *d1;
				d1=(char *)malloc(100*sizeof(char));
				strcat(d1,"diff ");
				strcat(d1,command->args[2]);
				strcat(d1," ");
				strcat(d1,command->args[3]);
				strcat(d1," > d1.txt");
				system(d1);
				char *d2;
				d2=(char *)malloc(100*sizeof(char));
				strcat(d2,"diff ");
				strcat(d2,command->args[2]);
				strcat(d2," ");
				strcat(d2,command->args[3]);
				strcat(d2," > d2.txt");
				system(d2);

				char const* const fileName = "d1.txt"; 
    				FILE* file = fopen(fileName, "r"); 
    				char line[256];
				int count = 0;
				int diff = 0;
    				while (fgets(line, sizeof(line), file)) {
					count++;
					if(line[0]<58 && line[0]>48){
						int first;
						int last;
						//int first = atoi(token);
						if(line[1] == 'c'){
							first = line[0] - 48;
							last = first;
						}
						else{
							char * token = strtok(line, "c");
							token = strtok(NULL,",");
							first = atoi(token);
							token = strtok(NULL,",");
							last = atoi(token);
						}  
						int numberOfLines = last - first + 1;
						for (int i = 0; i < numberOfLines; i++){
							diff++;
							printf("%s:Line %d: %s",command->args[2], first + i, findLine(count + 1 + i,"d2.txt"));
							printf("%s:Line %d: %s",command->args[3], first + i, findLine(count + 2 + i + numberOfLines,"d2.txt"));
						}
						
					}
    				}
				if(diff == 0){
					printf("The two files are identical\n");
				}
				else if(diff == 1){
					printf("1 differend line found\n");
				}
				else{
					printf("%d different lines found\n", diff);
				}
			    	fclose(file);
				system("rm d1.txt");
				system("rm d2.txt");
			}
		}
		else if (strcmp(command->args[0],"goodMorning") == 0){
			char arg_g[100];
			char *minute;
			char *hour;
			minute = (char *)malloc(2*sizeof(char));
			hour = (char *)malloc(2*sizeof(char));
			
			hour[0] = command->args[1][0];
			if (command->args[1][1]=='.'){
				minute[0] = command->args[1][2];
				minute[1] = command->args[1][3];
			}
			else {
				hour[1] = command->args[1][1];
				minute[0] = command->args[1][3];
				minute[1] = command->args[1][4];
			}

			strcpy(arg_g,"echo \"");
			strcat(arg_g,minute);
			strcat(arg_g," ");
			strcat(arg_g,hour);
			strcat(arg_g," * * * env DISPLAY=:0.0 rhythmbox-client --play ~");
			strcat(arg_g,command->args[2]+5);
			strcat(arg_g,"\" > crontemp.txt");
			system(arg_g);
			system("crontab -r");
			system("crontab crontemp.txt");
			system("rm crontemp.txt");
		}
		else if (strcmp(command->args[0],"highlight") == 0){
			char arg_c[100]="GREP_COLOR=0\"1;";
			if (strcmp(command->args[2],"r") == 0){
				strcat(arg_c,"31\" grep --color=always -R ");
				strcat(arg_c,command->args[1]);
				strcat(arg_c," ");
				strcat(arg_c,command->args[3]);
				}
			if (strcmp(command->args[2],"g") == 0){
				strcat(arg_c,"32\" grep --color=always -R ");
				strcat(arg_c,command->args[1]);
				strcat(arg_c," ");
				strcat(arg_c,command->args[3]);
				}
			if (strcmp(command->args[2],"b") == 0){
				strcat(arg_c,"96\" grep --color=always -R ");
				strcat(arg_c,command->args[1]);
				strcat(arg_c," ");
				strcat(arg_c,command->args[3]);
				}
			system(arg_c);
		} 
		else if (strcmp(command->args[0],"shortdir") == 0){
			if (strcmp(command->args[1],"set") == 0 && search_list("s.txt",command->args[2]) == 0){
				char arg[100]="echo '";
				strcat(arg,command->args[2]);
				char *buf;
				buf=(char *)malloc(100*sizeof(char));
				getcwd(buf,100);
				strcat(arg," ");
				strcat(arg,buf);
				strcat(arg,"' >> s.txt");
				system(arg);
			}	
			else if (strcmp(command->args[1],"list") == 0){
				if (access("s.txt", F_OK) == 0){
					system("cat s.txt");
				}
			}	
			else if (strcmp(command->args[1],"clear") == 0){
				if (access("s.txt", F_OK) == 0){
					system("rm s.txt");
				}
			}
			else if (strcmp(command->args[1],"del") == 0){
				int n = search_list("s.txt",command->args[2]);
				if (n == 0){
					printf("%s not found in name-directory associations.\n",command->args[2]);
				}
				else{
					int nLine = search_list("s.txt",command->args[2]);
					FILE *tempF;
					tempF = fopen("temp.txt", "w");
					FILE *s;
					s = fopen("s.txt", "r");
					char temp[512];
					int c = 0;
					while(fgets(temp, 512, s) != NULL) {
						c++;
						if(c != n){
							fprintf(tempF, "%s", temp);
						}
					}
				remove("s.txt");
				system("mv temp.txt s.txt");
				}
			}	
			else if (strcmp(command->args[1],"jump") == 0){
				if (access("s.txt", F_OK) == 0){
					char c[100];
  					FILE *fptr;
					fptr = fopen("s.txt", "r");
					fscanf(fptr, "%[^ ]", c);
					if(strcmp(c,command->args[2]) == 0){
						fscanf(fptr, "%[^\n]", c);
						char *d;
						d=(char *)malloc(100*sizeof(char));
						strcat(d,"echo '");
						strcat(d,c);
						strcat(d,"'");
						//system(d);
						fclose(fptr);
					} 
					else {
						char c_2[100];
	  					FILE *fptr_2;
						fptr_2 = fopen("s.txt", "r");

						fscanf(fptr_2, "%[^\n ] ", c_2);
						fscanf(fptr_2, "%[^\n ] ", c_2);
						fscanf(fptr_2, "%[^\n ] ", c_2);
						while(strcmp(c_2,command->args[2]) != 0){
							fscanf(fptr_2, "%[^\n ] ", c_2);
							fscanf(fptr_2, "%[^\n ] ", c_2);
						}
					fscanf(fptr_2, "%[^\n ] ", c_2);
					char *d;
					d=(char *)malloc(100*sizeof(char));
					//strcat(d,"cd ");
					//strcat(d,c_2);
					//printf("line:%s\n",d);
					//system(d);
					chdir("/home/ozan/Desktop");
					flag = 1;
					}	
				}
			}
		}
        	else {
            		const char *path_2;
            		path_2 = findPath(command->name);  
            		execv(path_2, command->args);      
        	}
        	exit(0);
	}
	else
	{
		if (!command->background){

			wait(0); // wait for child process to finish
			
		}
		return SUCCESS;
	}

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}

const char* findPath(char *cmd) {    

	FILE *fptr;
	char *path=malloc(100);
	char arg[100]="which ";
	strcat(arg,cmd);
	strcat(arg," > x.txt");  
                            
	system(arg);

	fptr = fopen ("x.txt", "r");  
	fgets(path,100,fptr);
	path[strlen(path)-1]='\0';
	system("rm x.txt");  
    	return path;
}

const char* findLine(int count, char *fname) {    

	char const* const fileName2 = fname;
	FILE* file2 = fopen(fileName2, "r");
	char *line2 = malloc(sizeof (char) * (512));
	int count2 = 0;
	for(int i = 0; i < count; i++) {
		fgets(line2, 512, file2);
	}
    	return line2;
}

int search_list(char *fname, char *str) { 
	FILE *fp;
	int find_result = 0;
	int res = 0;
	char temp[512];
	if((fp = fopen(fname, "r")) == NULL) {
		return(0);
	}
	while(fgets(temp, 512, fp) != NULL) {
		find_result++;
		if((strstr(temp, str)) != NULL) {
			res = find_result;
		}
	}
   	return(res);
}
