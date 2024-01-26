    #include "sh61.hh"
    #include <cstring>
    #include <cerrno>
    #include <vector>
    #include <sys/stat.h>
    #include <sys/wait.h>
    #include <algorithm>
    #define MAX 128

    // For the love of God
    #undef exit
    #define exit __DO_NOT_CALL_EXIT__READ_PROBLEM_SET_DESCRIPTION__

    // struct command
    //    Data structure describing a command.

    struct command {
        std::vector<std::string> args;

        pid_t pid = -1;      // process ID running this command, -1 if none
        char wd[MAX];

        command* next_in_pipeline;
        command* prev_in_pipeline;

        // handle redirections
        // redirection types
        int redir_in;
        int redir_out;
        int redir_err;

        // EC redirs
        int redir_append_out;
        int redir_append_err;

        bool file_open_err;

        int pfd[2];
        int redir_hold[5];

        std::string redir_file[5]; // files to redirect

        command();
    };

    char pwd[MAX];

    // command::command()
    //    This constructor function initializes a `command` structure.

    command::command() {
        this -> pid = -1;
        this -> redir_in = -1;
        this -> redir_out = -1;
        this -> redir_err = -1;
        this -> redir_append_err = -1;
        this -> redir_append_out = -1;
        this ->file_open_err = false;

        this -> next_in_pipeline = nullptr;
        this -> prev_in_pipeline = nullptr;
    }

    // struct pipeline
    // pipelines include any commands separated by "|"

    struct pipeline {
        command* command_child;
        pipeline* next_in_conditional;
        bool next_is_or;
        std::vector<std::string> children;
        pipeline();
    };

    pipeline::pipeline() {
        this->command_child = nullptr;
        this->next_in_conditional = nullptr;
        next_is_or = false;
    }

    // struct conditional
    // conditionals include pipelines that are separated by "&&" or "||"

    struct conditional {
        pipeline* pipeline_child = nullptr; // first pline
        conditional* next_in_list = nullptr;
        std::vector<pid_t> con_c_active; // collects all concurrent active children

        bool is_background = false;
        bool next = true;
        conditional();
    };

    conditional::conditional(){
        this->pipeline_child = nullptr;
        this->next_in_list = nullptr;
        is_background = false;
        next = true;
    }

    // used to clean possible leaks from new commands*
    void delete_all (conditional* con) {
        while (con != nullptr) {
            conditional* con_temp = con;
            pipeline* pip = con ->pipeline_child;
            while (pip != nullptr) {
                pipeline* temp_pip = pip;
                command* com = pip -> command_child;
                while (com!= nullptr) {
                    command* com_temp = com;
                    com = com->next_in_pipeline;
                    delete com_temp;
                }
                pip = pip-> next_in_conditional;
                delete temp_pip;
            }
            con = con->next_in_list;
            delete con_temp;
        }
    }

    struct foreground_pipeline {
        pid_t pgid;
        pipeline* pip;
    } foreground_pipeline;

    void sigint_handler(int signum) {
        // Check if there is a foreground pipeline
        if (foreground_pipeline.pgid != 0) {
            // Send the SIGINT signal to all processes in the foreground pipeline's process group
            killpg(foreground_pipeline.pgid, SIGINT);

            // Clear the foreground pipeline
            foreground_pipeline.pgid = 0;
            foreground_pipeline.pip = nullptr;
        }
    }

    // COMMAND EXECUTION

    // void command::run() --> Implemented within run_pipeline()

    // int run_pipeline()
    // this function pipes between commands within pline, as well as
    // runs commands at the same time. Returns value 0 to conditional function
    //

    int run_pipeline(pipeline* pip) {
        
        command* com = pip->command_child; // current command
        std::vector<pid_t> c_running; // collects PIDS for zombie collection

        int pfd[2]; // pipe to next
        int prev_pfd[2] = {-1, -1}; // pipe from prev
        int status = 0; //ret value

        if (pip == nullptr) {
            return 0;
        }

        while (true){
            assert(com);
            // handle cd
            if (com->args[0].compare("cd") == 0){
                if(com->args.size() == 2){
                    if(chdir(com->args[1].c_str())!=0) {
                        assert(chdir(pwd));
                        ++status;
                    }
                    // copy wd to pwd
                    strncpy(pwd,com->wd,sizeof(com->wd));
                }
                else {
                    // if cd managed incorrectly, increment status for error
                    ++status;
                }
                if (!com->next_in_pipeline) {
                    // break if no command
                    break;
                }
                com = com->next_in_pipeline;
                prev_pfd[0] = pfd[0];
                prev_pfd[1] = pfd[1];
            }

            if (com->redir_in == 0){
                com->redir_hold[0] = open(com->redir_file[0].c_str(), O_RDONLY);
                if (com->redir_hold[0] < 0){
                    // open read file
                    com->file_open_err = true;
                    fprintf(stderr, "No such file or directory\n");
                }
            }

            if (com-> redir_out == 0) {
                com->redir_hold[1] = open(com->redir_file[1].c_str(), O_WRONLY|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
                if (com->redir_hold[1] < 0) {
                    // open specified out file
                    com->file_open_err = true;
                    fprintf(stderr, "No such file or directory\n");
                }
            }
            if (com-> redir_err == 0) {
                com->redir_hold[2] = open(com->redir_file[2].c_str(), O_WRONLY|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
                if (com->redir_hold[2] < 0) {
                    // open specified err file
                    com->file_open_err = true;
                    fprintf(stderr, "No such file or directory\n");
                }
            }       

            // EC Redir
            if (com-> redir_append_out == 0) {
                com->redir_hold[3] = open(com->redir_file[3].c_str(), O_WRONLY|O_CREAT|O_APPEND, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
                if (com->redir_hold[3] < 0) {
                    // open specified err file
                    com->file_open_err = true;
                    fprintf(stderr, "No such file or directory\n");
                }
            } 
            if (com-> redir_append_err == 0) {
                com->redir_hold[4] = open(com->redir_file[4].c_str(), O_WRONLY | O_CREAT | O_APPEND, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
                if (com->redir_hold[4] < 0) {
                    // open specified err file
                    com->file_open_err = true;
                    fprintf(stderr, "No such file or directory\n");
                }
            } 

            if (com->file_open_err) {
                ++status;
                break;
            }

            // create new pipe in parent
            int rr = pipe(pfd);
            assert(rr >= 0);

            // fork pipe to child
            pid_t p = fork();
            assert (p >= 0);
            // child
            if (p == 0){
                if (com->next_in_pipeline && com->redir_out == -1) {
                    dup2 (pfd[1], 1);
                }
                close (pfd[1]);
                close (pfd[0]);
                        
                if (prev_pfd[0] != -1 && com->redir_in == -1) {
                    dup2 (prev_pfd[0], 0);
                }
                close (prev_pfd[0]);

                if(com->redir_in == 0) {
                    dup2(com->redir_hold[0],0);
                    close(com->redir_hold[0]);
                }
                if(com->redir_out == 0) {
                    dup2(com->redir_hold[1],1);
                    close(com->redir_hold[1]);
                }
                if(com->redir_err == 0) {
                    dup2(com->redir_hold[2],2);
                    close(com->redir_hold[2]);
                }
                if(com->redir_append_out == 0) {
                    dup2(com->redir_hold[3],3);
                    close(com->redir_hold[3]);
                }              
                if(com->redir_append_err == 0) {
                    dup2(com->redir_hold[4],4);
                    close(com->redir_hold[4]);
                }

                // copy args vector to args array since execvp takes null termniated array
                std::vector<const char *> argv;
                for (auto it = com->args.begin(); it !=  com->args.end(); ++it) {
                    argv.push_back((char*)it->c_str());
                }
                argv.push_back(nullptr);

                if (argv[0] == nullptr) {
                    _exit(EXIT_FAILURE);
                }

                execvp(argv[0], (char *const *) argv.data());
                _exit(EXIT_FAILURE);
            }
            
            //parent
            else if (p > 0) {
                close(pfd[1]);
                if (prev_pfd[0] != -1) {
                    close (prev_pfd[0]);
                }
                if(com->redir_in == 0) {
                    close(com->redir_hold[0]);
                }
                if(com->redir_out == 0) {
                    close(com->redir_hold[1]);
                }
                if(com->redir_err == 0) {
                    close(com-> redir_hold[2]);
                }
                if(com->redir_append_out == 0) {
                close(com-> redir_hold[3]);
                }
                if(com->redir_append_err == 0) {
                close(com-> redir_hold[4]);
                }
                // account for child pids
                c_running.push_back(p);
            }
            else {
                status++;
            }
            // if no next command, end loop
            if (!com->next_in_pipeline) {
                break;
            }
            else {
            com = com->next_in_pipeline;
            prev_pfd[0] = pfd[0];
            prev_pfd[1] = pfd[1];
            }
        }

        // collect status/clean
        int status_check = 0;
        int status_2 = 0;
        int clean;

        while(c_running.begin() != c_running.end()) {
            clean = waitpid(*c_running.begin(),&status_check,WNOHANG);
            assert(clean>=0);
            if(clean > 0) {
                c_running.erase(c_running.begin());
            }
        }
        if(!WIFEXITED(status_check) || (WEXITSTATUS(status_check) != 0)) {
            ++status_2;
        }
        // equals 0 iff redirect and exit is successful
        return (status + status_2);
    }


    // run_conditional()
    // runs individual pipelines in conditional.

    int run_conditional(conditional* con) {
        int reply = 0; // ret val from pline
        pipeline* pip = con->pipeline_child; // first pline
        while (true) {
            // run pipeline if true
            if (con->next) {
                reply = run_pipeline(pip);
            }
            if (pip->next_is_or == reply) {
                con->next=true;
            }
            else {
                con->next=false;
            }
            if (!pip->next_in_conditional){
                break;
            }
            pip = pip->next_in_conditional;
        }
        return reply;
    }

    // run_list(conditional* con)
    //    Run the conditional starting at `con`. This extends to handle command lists,
    //    conditionals, and pipelines.
    //

    void run_list(conditional* con) {
        std::vector<pid_t> c_running; // track cpids
        std::vector<conditional*> bg_conds; // commands to run in background
        while(true) {
            if (con->is_background) {
                // collect background processes
                while (true) {
                    bg_conds.push_back(con);
                    if(!con->next_in_list) {
                        break;
                    }
                    if (!(con->next_in_list->is_background)) {
                        break;
                    }
                    con = con->next_in_list;
                }

                // run background conditionals
                for (auto iter = bg_conds.begin(); iter != bg_conds.end(); ++iter) {

                    // fork cond to child
                    pid_t p_2 = fork();
                    if (p_2 == 0) {
                        // run background conditional
                        int r = run_conditional(*iter);
                        assert (r>=0);
                        _exit(0);
                    }
                    if (p_2 > 0) {
                        // keep track of active children
                        c_running.push_back(p_2);
                    }

                }
                // parent clears background list
                while(bg_conds.begin() != bg_conds.end()) {
                    bg_conds.erase(bg_conds.begin());
                }
            }

            else {
                int r = run_conditional(con);
                assert(r>=0);
            }

            // step to next conditional iff it exists
            if(!con->next_in_list) {
                break;
            }
            con = con->next_in_list;
        }
    }

    // parse_line(s)
    //    Parse the command list in `s` and return it. Returns `nullptr` if
    //    `s` is empty (only spaces). You’ll extend it to handle more token
    //    types.

    conditional* parse_line(const char* s) {
        shell_parser parser(s);

        // Build the command
        command*     cmd_head = nullptr;    // first command
        command*     cmd_cur  = nullptr;    // current command being built
        command*     cmd_last = nullptr;    // command before current

        conditional* con_head = nullptr;    // first conditional in list
        conditional* con_cur  = nullptr;    // current conditional being built
        conditional* con_last = nullptr;    // conditional before current

        pipeline*    pipe_head = nullptr;    // first pipeline in conditional
        pipeline*    pipe_cur  = nullptr;    // current pipeline being built
        pipeline*    pipe_last = nullptr;    // pipeline before current

        // for file redirects
        bool next_redir = false;
        // for cd
        bool new_dir = false;

        std::string redir; // for types of redirections

        for (auto iter = parser.begin(); iter != parser.end(); ++iter) {
            switch (iter.type()) {

            case TYPE_NORMAL:
                if (!con_cur) {
                    con_cur = new conditional();
                    assert(con_cur);
                    // prev points to new cond
                    if (con_last) {
                        con_last->next_in_list = con_cur;
                    }
                    else {
                        con_head = con_cur;
                    }
                }

                if (!pipe_cur) {
                    pipe_cur = new pipeline();
                    assert(pipe_cur);
                    if (pipe_last) {
                        pipe_last->next_in_conditional = pipe_cur;
                    }
                    else {
                        pipe_head = pipe_cur;
                        con_cur->pipeline_child = pipe_head;
                    }
                }

                if (!cmd_cur) {
                    cmd_cur = new command();
                    assert(cmd_cur);
                    if (cmd_last) {
                        cmd_last->next_in_pipeline = cmd_cur;
                        cmd_cur->prev_in_pipeline = cmd_last;
                    }
                    else {
                        cmd_head = cmd_cur;
                        pipe_cur->command_child = cmd_head;
                    }
                }

                if (next_redir) {
                    if(strcmp(redir.c_str(),"2>>") == 0) {
                        cmd_cur->redir_append_err = 0;
                        cmd_cur->redir_file[4] = iter.str();
                    }
                    else if(strcmp(redir.c_str(),">>") == 0) {
                        cmd_cur->redir_append_out = 0;
                        cmd_cur->redir_file[3] = iter.str();
                    }
                    //     add redir token and read file to current command
                    else if(strcmp(redir.c_str(),"<" ) == 0) {
                        cmd_cur->redir_in = 0;
                        cmd_cur->redir_file[0] = iter.str();
                    }
                    else if(strcmp(redir.c_str(),">" ) == 0) {
                        cmd_cur->redir_out = 0;
                        cmd_cur->redir_file[1] = iter.str();
                    }
                    else if(strcmp(redir.c_str(),"2>") == 0) {
                        cmd_cur->redir_err = 0;
                        cmd_cur->redir_file[2] = iter.str();
                    }
                    next_redir = false;
                    break;
                }

                if (iter.str().compare("cd") == 0) {
                    new_dir = true;
                }

                if (new_dir) {
                    // write the next Normal as the destination
                    strncpy(cmd_cur->wd, iter.str().c_str(), strlen(iter.str().c_str()));
                    cmd_cur->wd[strlen(iter.str().c_str())] = '\0';
                    new_dir = false;
                }

                cmd_cur->args.push_back(iter.str()); // Add argument to current command
                break;

            case TYPE_REDIRECT_OP:
                assert(!next_redir); // previous should not be redirect
                
                redir = iter.str(); // save redir for next
                next_redir = true;
                break;

            case TYPE_SEQUENCE:
            case TYPE_BACKGROUND:
            case TYPE_AND:
            case TYPE_OR:
            case TYPE_PIPE:
                // previous should not be redirect
                assert(!next_redir);

                // set flags
                if (iter.type() == TYPE_BACKGROUND) {
                    con_cur->is_background = true;
                }
                if (iter.type() == TYPE_SEQUENCE) {
                    con_cur->is_background = false;
                }
                if (iter.type() == TYPE_OR) {
                    pipe_cur->next_is_or = true;
                }
                if (iter.type() == TYPE_AND) {
                    pipe_cur->next_is_or = false;
                }

                // end the current command and set prev
                cmd_last = cmd_cur;
                cmd_cur = nullptr;
                if (iter.type() == TYPE_PIPE) {
                    break;
                }

                cmd_head = nullptr;
                cmd_last = nullptr;

                // end current pipeline
                pipe_last = pipe_cur;
                pipe_cur = nullptr;
                
                if (iter.type() == TYPE_AND || iter.type() == TYPE_OR) {
                    break;
                }

                // new pline and no previous
                pipe_head = nullptr;
                pipe_last = nullptr;

                // end current conditional
                con_last = con_cur;
                con_cur = nullptr;
                break;
            }
        }
        // return first conditional
        return con_head;
    }



    int main(int argc, char* argv[]) {
        FILE* command_file = stdin;
        bool quiet = false;

        // Check for `-q` option: be quiet (print no prompts)
        if (argc > 1 && strcmp(argv[1], "-q") == 0) {
            quiet = true;
            --argc, ++argv;
        }

        // Check for filename option: read commands from file
        if (argc > 1) {
            command_file = fopen(argv[1], "rb");
            if (!command_file) {
                perror(argv[1]);
                return 1;
            }
        }

        // - Put the shell into the foreground
        // - Ignore the SIGTTOU signal, which is sent when the shell is put back
        //   into the foreground
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigint_handler;
        sigaction(SIGINT, &sa, NULL);
        claim_foreground(0);
        set_signal_handler(SIGTTOU, SIG_IGN);

        char buf[BUFSIZ];
        int bufpos = 0;
        bool needprompt = true;

        while (!feof(command_file)) {
            // Print the prompt at the beginning of the line
            if (needprompt && !quiet) {
                printf("sh61[%d]$ ", getpid());
                fflush(stdout);
                needprompt = false;
            }

            // Read a string, checking for error or EOF
            if (fgets(&buf[bufpos], BUFSIZ - bufpos, command_file) == nullptr) {
                if (ferror(command_file) && errno == EINTR) {
                    // ignore EINTR errors
                    clearerr(command_file);
                    buf[bufpos] = 0;
                } else {
                    if (ferror(command_file)) {
                        perror("sh61");
                    }
                    break;
                }
            }

            // If a complete command line has been provided, run it
            bufpos = strlen(buf);
            if (bufpos == BUFSIZ - 1 || (bufpos > 0 && buf[bufpos - 1] == '\n')) {
                if (conditional* c = parse_line(buf)) {
                    run_list(c);
                    delete_all (c);
                }
                bufpos = 0;
                needprompt = 1;
            }

            // Handle zombie processes and/or interrupt requests
            int status_check;
            while(waitpid(-1,&status_check,WNOHANG) > 0) {}
        }
        return 0;
    }





