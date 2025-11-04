#include <bits/stdc++.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctime>
#include <pwd.h>
using namespace std;

struct Command {
    vector<string> args;
    string infile;
    string outfile;
};

static vector<pid_t> bg_pids;
static string previous_dir = "";

void reap_background() {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        auto it = find(bg_pids.begin(), bg_pids.end(), pid);
        if (it != bg_pids.end()) bg_pids.erase(it);
    }
}

void print_prompt() {
    char timebuf[64];
    time_t t = time(nullptr);
    struct tm *tm_info = localtime(&t);
    strftime(timebuf, sizeof(timebuf), "%b %d %H:%M:%S", tm_info);
    const char* user = getenv("USER");
    if (!user) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) user = pw->pw_name;
    }
    if (!user) user = "user";
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
    cout << timebuf << " " << user << ":" << cwd << "$ ";
    cout.flush();
}

vector<string> tokenize(const string &line) {
    vector<string> tokens;
    string cur;
    bool in_single = false, in_double = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\'' && !in_double) {
            in_single = !in_single;
            continue;
        }
        if (c == '\"' && !in_single) {
            in_double = !in_double;
            continue;
        }
        if (!in_single && !in_double) {
            if (isspace((unsigned char)c)) {
                if (!cur.empty()) {
                    tokens.push_back(cur);
                    cur.clear();
                }
                continue;
            }
            if (c == '|' || c == '<' || c == '>' || c == '&') {
                if (!cur.empty()) {
                    tokens.push_back(cur);
                    cur.clear();
                }
                string s(1, c);
                tokens.push_back(s);
                continue;
            }
        }
        cur.push_back(c);
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

bool parse_line(const string &line, vector<Command> &commands, bool &background) {
    commands.clear();
    background = false;
    vector<string> tokens = tokenize(line);
    if (tokens.empty()) return false;
    if (!tokens.empty() && tokens.back() == "&") {
        background = true;
        tokens.pop_back();
    }
    Command curcmd;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const string &tok = tokens[i];
        if (tok == "|") {
            if (curcmd.args.empty()) continue;
            commands.push_back(curcmd);
            curcmd = Command();
        } else if (tok == "<") {
            if (i + 1 < tokens.size()) {
                curcmd.infile = tokens[i + 1];
                ++i;
            }
        } else if (tok == ">") {
            if (i + 1 < tokens.size()) {
                curcmd.outfile = tokens[i + 1];
                ++i;
            }
        } else {
            curcmd.args.push_back(tok);
        }
    }
    if (!curcmd.args.empty() || !curcmd.infile.empty() || !curcmd.outfile.empty()) {
        commands.push_back(curcmd);
    }
    return !commands.empty();
}

vector<char*> make_argv(const vector<string> &args) {
    vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const string &s : args) {
        char *c = (char*)malloc(s.size() + 1);
        strcpy(c, s.c_str());
        argv.push_back(c);
    }
    argv.push_back(nullptr);
    return argv;
}

void free_argv(vector<char*> &argv) {
    for (char* p : argv) {
        if (p) free(p);
    }
}

void execute_pipeline(vector<Command> &commands, bool background) {
    size_t n = commands.size();
    if (n == 0) return;
    vector<int> pipes;
    if (n > 1) {
        pipes.resize(2 * (n - 1));
        for (size_t i = 0; i < n - 1; ++i) {
            if (pipe(&pipes[2*i]) == -1) {
                perror("pipe");
                for (size_t j = 0; j < 2*i; ++j) close(pipes[j]);
                return;
            }
        }
    }
    vector<pid_t> pids;
    pids.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            for (int fd : pipes) close(fd);
            return;
        }
        if (pid == 0) {
            if (!commands[i].infile.empty()) {
                int infd = open(commands[i].infile.c_str(), O_RDONLY);
                if (infd == -1) {
                    perror(("open infile " + commands[i].infile).c_str());
                    _exit(1);
                }
                if (dup2(infd, STDIN_FILENO) == -1) {
                    perror("dup2 infile");
                    _exit(1);
                }
                close(infd);
            } else if (i > 0) {
                int read_fd = pipes[(i-1)*2];
                if (dup2(read_fd, STDIN_FILENO) == -1) {
                    perror("dup2 pipe read");
                    _exit(1);
                }
            }
            if (!commands[i].outfile.empty()) {
                int outfd = open(commands[i].outfile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (outfd == -1) {
                    perror(("open outfile " + commands[i].outfile).c_str());
                    _exit(1);
                }
                if (dup2(outfd, STDOUT_FILENO) == -1) {
                    perror("dup2 outfile");
                    _exit(1);
                }
                close(outfd);
            } else if (i < n - 1) {
                int write_fd = pipes[i*2 + 1];
                if (dup2(write_fd, STDOUT_FILENO) == -1) {
                    perror("dup2 pipe write");
                    _exit(1);
                }
            }
            for (int fd : pipes) close(fd);
            vector<char*> argv = make_argv(commands[i].args);
            execvp(argv[0], argv.data());
            perror(("execvp " + commands[i].args[0]).c_str());
            free_argv(argv);
            _exit(1);
        } else {
            pids.push_back(pid);
        }
    }
    for (int fd : pipes) close(fd);
    if (background) {
        for (pid_t pid : pids) bg_pids.push_back(pid);
    } else {
        for (pid_t pid : pids) {
            int status = 0;
            waitpid(pid, &status, 0);
        }
    }
}

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd))) previous_dir = string(cwd);
    string line;
    while (true) {
        reap_background();
        print_prompt();
        if (!getline(cin, line)) {
            cout << "\n";
            break;
        }
        auto l = line.find_first_not_of(" \t\r\n");
        if (l == string::npos) continue;
        auto r = line.find_last_not_of(" \t\r\n");
        string trimmed = line.substr(l, r - l + 1);
        if (trimmed.empty()) continue;
        vector<Command> commands;
        bool background = false;
        bool parsed = parse_line(trimmed, commands, background);
        if (!parsed) continue;
        if (commands.size() == 1 && !commands[0].args.empty()) {
            string cmd0 = commands[0].args[0];
            if (cmd0 == "exit") {
                reap_background();
                break;
            } else if (cmd0 == "cd") {
                string target;
                if (commands[0].args.size() >= 2) {
                    target = commands[0].args[1];
                } else {
                    const char* home = getenv("HOME");
                    if (home) target = string(home);
                    else target = string("/");
                }
                if (target == "-") {
                    if (previous_dir.empty()) {
                        cerr << "cd: OLDPWD not set\n";
                    } else {
                        char curdir[4096];
                        if (getcwd(curdir, sizeof(curdir))) {
                            if (chdir(previous_dir.c_str()) == -1) {
                                perror(("cd " + previous_dir).c_str());
                            } else {
                                cout << previous_dir << "\n";
                                previous_dir = string(curdir);
                            }
                        } else {
                            if (chdir(previous_dir.c_str()) == -1) {
                                perror(("cd " + previous_dir).c_str());
                            } else {
                                cout << previous_dir << "\n";
                                previous_dir = string("/");
                            }
                        }
                    }
                } else {
                    char curdir[4096];
                    if (getcwd(curdir, sizeof(curdir))) previous_dir = string(curdir);
                    if (chdir(target.c_str()) == -1) {
                        perror(("cd " + target).c_str());
                    }
                }
                continue;
            }
        }
        execute_pipeline(commands, background);
    }
    return 0;
}
