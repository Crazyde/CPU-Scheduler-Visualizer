
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <queue>
#include <deque>
#include <map>
#include <sstream>
#include <iomanip>
#include <numeric>
using namespace std;

// ---------------- Types ----------------
struct Process {
    string name;
    int arrival;
    int service;
    int priority;
};

// ---------------- Globals (per run) ----------------
vector<Process> processes;
int process_count = 0;

// Note: input last_instant is used ONLY to size internal buffers safely.
// We *export* last_instant at the end as the ACTUAL used time.
int last_instant = 0;

string operation = "TRACE";  // TRACE or STATS
int globalQuantum = 2;
bool priority_low_to_high = true;

// Time series buffers
vector<vector<char>> timeline;     // timeline[t][i]: '*': running, '.': waiting, ' ': empty
vector<int> finishTime, turnAroundTime, waitTime, responseTime, remainingTime;
vector<double> normTurn;
vector<string> ganttChart;         // per time-unit name of the running job ("Idle" if we emitted idle)
vector<string> timelinePerProcess; // compact rows for JSON
vector<vector<string>> readyQueues;// readyQueues[t] = vector of process names ready (waiting) at time t

// ---------------- Input parsing ----------------
string algorithm_chunk;
char selectedAlgoId = '1';
int selectedAlgoQuantum = -1;

void parse_algorithms(const string &chunk, vector<pair<char,int>>& algorithms) {
    stringstream stream(chunk);
    while (stream.good()) {
        string temp_str;
        getline(stream, temp_str, ',');
        if (temp_str.empty()) continue;
        stringstream ss(temp_str);
        string idpart; getline(ss, idpart, '-');
        char algorithm_id = idpart.size() ? idpart[0] : '1';
        string qpart;   getline(ss, qpart, '-');
        int quantum = qpart.size() ? stoi(qpart) : -1;
        algorithms.push_back({algorithm_id, quantum});
    }
}

void parse_input() {
    // stdin format:
    // operation algorithm_chunk last_instant process_count priority_order
    // then process_count lines: name,arrival,service[,priority]
    string priority_order_str;
    if (!(cin >> operation >> algorithm_chunk >> last_instant >> process_count >> priority_order_str)) {
        cerr << "Invalid input format\n";
        exit(1);
    }
    priority_low_to_high = (priority_order_str == "lower");

    vector<pair<char,int>> algorithms;
    parse_algorithms(algorithm_chunk, algorithms);
    if (!algorithms.empty()) {
        selectedAlgoId = algorithms[0].first;
        selectedAlgoQuantum = algorithms[0].second;
    } else {
        selectedAlgoId = '1';
        selectedAlgoQuantum = -1;
    }

    processes.clear();
    for (int i = 0; i < process_count; ++i) {
        string line;
        if (!(cin >> line)) { cerr << "Missing process line\n"; exit(1); }
        stringstream ss(line);
        Process p; string temp;
        getline(ss, temp, ','); p.name     = temp;
        getline(ss, temp, ','); p.arrival  = stoi(temp);
        getline(ss, temp, ','); p.service  = stoi(temp);
        if (getline(ss, temp, ',')) p.priority = stoi(temp); else p.priority = 0;
        processes.push_back(p);
    }
}

// ---------------- Helpers ----------------

// Ensure internal buffers are large enough to simulate safely.
// NOTE: We will TRIM the *exported* last_instant to actual used time later.
void computeLastInstant() {
    int totalService = 0, maxArrival = 0;
    for (auto &p : processes) {
        totalService += p.service;
        maxArrival = max(maxArrival, p.arrival);
    }
    int minNeeded = maxArrival + totalService + 2; // small slack
    if (last_instant < minNeeded) last_instant = minNeeded;
}

void prepareRun() {
    computeLastInstant();
    timeline.assign(last_instant, vector<char>(process_count, ' '));
    finishTime.assign(process_count, 0);
    turnAroundTime.assign(process_count, 0);
    waitTime.assign(process_count, 0);
    responseTime.assign(process_count, -1);
    remainingTime.assign(process_count, 0);
    normTurn.assign(process_count, 0.0);
    ganttChart.clear();
    timelinePerProcess.assign(process_count, string(last_instant, ' '));
    readyQueues.clear();
    for (int i=0;i<process_count;i++) remainingTime[i] = processes[i].service;
}

// Fill '.' for waiting intervals between arrival and finish where not running
void fillInWaitMarkers() {
    for (int i = 0; i < process_count; ++i) {
        int arr = processes[i].arrival;
        int fin = finishTime[i];
        if (fin == 0) continue;
        for (int t = arr; t < fin && t < last_instant; ++t) {
            if (timeline[t][i] != '*') timeline[t][i] = '.';
        }
    }
}

void computeStats() {
    for (int i = 0; i < process_count; ++i) {
        int arr = processes[i].arrival;
        int svc = processes[i].service;
        int fin = finishTime[i];
        if (fin == 0) {
            turnAroundTime[i] = waitTime[i] = 0;
            if (responseTime[i] == -1) responseTime[i] = -1;
            normTurn[i] = 0.0;
        } else {
            turnAroundTime[i] = fin - arr;
            waitTime[i] = turnAroundTime[i] - svc;
            if (responseTime[i] == -1) responseTime[i] = 0;
            normTurn[i] = double(turnAroundTime[i]) / double(svc);
        }
    }
}

inline bool higherPriority(int p1, int p2) {
    return priority_low_to_high ? (p1 < p2) : (p1 > p2);
}

// Utility: build a ready snapshot for time t, excluding 'excludeIdx'
vector<string> buildReadySnapshot(int t, int excludeIdx) {
    vector<string> snap;
    for (int i=0;i<process_count;i++) {
        if (i == excludeIdx) continue;
        if (remainingTime[i] > 0 && processes[i].arrival <= t) {
            snap.push_back(processes[i].name);
        }
    }
    return snap;
}

// ---------------- Algorithms ----------------

// 1) FCFS (Non-preemptive)
void firstComeFirstServe() {
    prepareRun();
    vector<int> idx(process_count);
    iota(idx.begin(), idx.end(), 0);
    stable_sort(idx.begin(), idx.end(), [&](int a, int b){
        if (processes[a].arrival != processes[b].arrival) return processes[a].arrival < processes[b].arrival;
        return a < b;
    });

    int time = 0;
    for (int id : idx) {
        // Jump to this job's arrival if CPU was "idle" before it — we do NOT emit idle ticks.
        time = max(time, processes[id].arrival);

        for (int t = time; t < time + processes[id].service && t < last_instant; ++t) {
            // Compute snapshot before executing this quantum to reflect what's waiting right now.
            vector<string> snap = buildReadySnapshot(t, id);
            readyQueues.push_back(snap);

            timeline[t][id] = '*';
            ganttChart.push_back(processes[id].name);
            timelinePerProcess[id][t] = '*';
            if (responseTime[id] == -1) responseTime[id] = t - processes[id].arrival;
            remainingTime[id]--;
        }
        finishTime[id] = time + processes[id].service;
        time += processes[id].service;
    }
    fillInWaitMarkers();
    computeStats();
}

// 2) SJF (Non-preemptive)
void shortestJobFirst_nonpreemptive() {
    prepareRun();
    int time = 0, done = 0;
    vector<bool> doneFlag(process_count,false);
    while (done < process_count) {
        int best = -1;
        for (int i=0;i<process_count;i++) {
            if (!doneFlag[i] && processes[i].arrival <= time) {
                if (best==-1 || processes[i].service < processes[best].service) best = i;
            }
        }
        if (best == -1) {
            // Emit explicit idle tick here (SJF advances one by one if nothing is ready)
            ganttChart.push_back("Idle");
            readyQueues.push_back(vector<string>()); // nothing waiting
            time++;
            continue;
        }
        for (int t = time; t < time + processes[best].service && t < last_instant; ++t) {
            vector<string> snap = buildReadySnapshot(t, best);
            readyQueues.push_back(snap);

            timeline[t][best] = '*';
            ganttChart.push_back(processes[best].name);
            timelinePerProcess[best][t] = '*';
            if (responseTime[best] == -1) responseTime[best] = t - processes[best].arrival;
            remainingTime[best]--;
        }
        time += processes[best].service;
        finishTime[best] = time;
        doneFlag[best] = true;
        done++;
    }
    fillInWaitMarkers();
    computeStats();
}

// 3) SRTF (Preemptive)
void srtf_preemptive() {
    prepareRun();
    int time=0, done=0;
    while (done < process_count) {
        int best=-1;
        for (int i=0;i<process_count;i++) {
            if (remainingTime[i] > 0 && processes[i].arrival <= time) {
                if (best==-1 || remainingTime[i] < remainingTime[best]) best = i;
            }
        }
        if (best==-1) {
            ganttChart.push_back("Idle");
            readyQueues.push_back(vector<string>());
            time++; continue;
        }
        vector<string> snap = buildReadySnapshot(time, best);
        readyQueues.push_back(snap);

        timeline[time][best] = '*';
        ganttChart.push_back(processes[best].name);
        timelinePerProcess[best][time] = '*';
        if (responseTime[best] == -1) responseTime[best] = time - processes[best].arrival;
        remainingTime[best]--;

        if (remainingTime[best]==0) { finishTime[best] = time + 1; done++; }
        time++;
    }
    fillInWaitMarkers();
    computeStats();
}

// 4) Priority (Non-preemptive)
void priority_nonpreemptive() {
    prepareRun();
    int time=0, done=0;
    vector<bool> doneFlag(process_count,false);
    while (done < process_count) {
        int best=-1;
        for (int i=0;i<process_count;i++) {
            if (!doneFlag[i] && processes[i].arrival <= time) {
                if (best==-1 || higherPriority(processes[i].priority, processes[best].priority)
                    || (processes[i].priority == processes[best].priority && processes[i].arrival < processes[best].arrival))
                    best = i;
            }
        }
        if (best==-1) { ganttChart.push_back("Idle"); readyQueues.push_back(vector<string>()); time++; continue; }
        for (int t=time; t<time+processes[best].service && t<last_instant; ++t) {
            vector<string> snap = buildReadySnapshot(t, best);
            readyQueues.push_back(snap);

            timeline[t][best] = '*';
            ganttChart.push_back(processes[best].name);
            timelinePerProcess[best][t] = '*';
            if (responseTime[best]==-1) responseTime[best] = t - processes[best].arrival;
            remainingTime[best]--;
        }
        time += processes[best].service;
        finishTime[best] = time;
        doneFlag[best] = true;
        done++;
    }
    fillInWaitMarkers();
    computeStats();
}

// 5) Priority (Preemptive)
void priority_preemptive() {
    prepareRun();
    int time=0, done=0;
    while (done < process_count) {
        int best=-1;
        for (int i=0;i<process_count;i++) {
            if (remainingTime[i] > 0 && processes[i].arrival <= time) {
                if (best==-1 || higherPriority(processes[i].priority, processes[best].priority)) best = i;
            }
        }
        if (best==-1) { ganttChart.push_back("Idle"); readyQueues.push_back(vector<string>()); time++; continue; }

        vector<string> snap = buildReadySnapshot(time, best);
        readyQueues.push_back(snap);

        timeline[time][best] = '*';
        ganttChart.push_back(processes[best].name);
        timelinePerProcess[best][time] = '*';
        if (responseTime[best]==-1) responseTime[best] = time - processes[best].arrival;
        remainingTime[best]--;
        if (remainingTime[best]==0) { finishTime[best] = time+1; done++; }
        time++;
    }
    fillInWaitMarkers();
    computeStats();
}

// 6) Round Robin (Preemptive, time-sliced)
// We emit a snapshot before each executed time unit (queue excluding running).
void roundRobin_timeSliced(int quantum) {
    prepareRun();
    deque<int> q;
    vector<bool> arrived(process_count,false);
    int time = 0;

    // enqueue arrivals at time 0
    for (int i=0;i<process_count;i++) {
        if (processes[i].arrival <= time && remainingTime[i] > 0) { q.push_back(i); arrived[i]=true; }
    }

    while (true) {
        if (q.empty()) {
            int nxt = -1;
            for (int i=0;i<process_count;i++) if (remainingTime[i]>0) {
                if (nxt==-1 || processes[i].arrival < processes[nxt].arrival) nxt = i;
            }
            if (nxt == -1) break; // all done
            time = max(time, processes[nxt].arrival);
            for (int i=0;i<process_count;i++) {
                if (!arrived[i] && processes[i].arrival <= time && remainingTime[i] > 0) { q.push_back(i); arrived[i]=true; }
            }
        }
        int cur = q.front(); q.pop_front();
        int slice = min(quantum, remainingTime[cur]);

        for (int t = time; t < time + slice && t < last_instant; ++t) {
            // Bring in arrivals that appear exactly at this t
            for (int j=0;j<process_count;j++) {
                if (!arrived[j] && processes[j].arrival <= t && remainingTime[j] > 0) { q.push_back(j); arrived[j] = true; }
            }
            // Snapshot: queue content (names) *as waiting list* (exclude running 'cur')
            vector<string> snap;
            for (int id : q) snap.push_back(processes[id].name);
            readyQueues.push_back(snap);

            // Execute one tick
            timeline[t][cur] = '*';
            ganttChart.push_back(processes[cur].name);
            timelinePerProcess[cur][t] = '*';
            if (responseTime[cur] == -1) responseTime[cur] = t - processes[cur].arrival;
            remainingTime[cur]--;
        }

        time += slice;
        for (int j=0;j<process_count;j++) {
            if (!arrived[j] && processes[j].arrival <= time && remainingTime[j] > 0) { q.push_back(j); arrived[j] = true; }
        }

        if (remainingTime[cur] > 0) { q.push_back(cur); }
        else { finishTime[cur] = time; }
    }
    fillInWaitMarkers();
    computeStats();
}

// 7) HRRN (Non-preemptive)
void hrrn_nonpreemptive() {
    prepareRun();
    int time=0, done=0;
    vector<bool> completed(process_count,false);
    while (done < process_count) {
        vector<int> ready;
        for (int i=0;i<process_count;i++) if (!completed[i] && processes[i].arrival <= time) ready.push_back(i);
        if (ready.empty()) { ganttChart.push_back("Idle"); readyQueues.push_back(vector<string>()); time++; continue; }

        int best = -1; double bestRR = -1.0;
        for (int idx : ready) {
            int wait = time - processes[idx].arrival;
            double rr = double(wait + processes[idx].service) / double(processes[idx].service);
            if (rr > bestRR) { bestRR = rr; best = idx; }
        }

        for (int t=time; t<time+processes[best].service && t<last_instant; ++t) {
            vector<string> snap = buildReadySnapshot(t, best);
            readyQueues.push_back(snap);

            timeline[t][best] = '*';
            ganttChart.push_back(processes[best].name);
            timelinePerProcess[best][t] = '*';
            if (responseTime[best]==-1) responseTime[best] = t - processes[best].arrival;
            remainingTime[best]--;
        }
        time += processes[best].service;
        finishTime[best] = time;
        completed[best] = true; done++;
    }
    fillInWaitMarkers();
    computeStats();
}

// 8) MLFQ (first-level example)
void mlfq_firstlevel() {
    prepareRun();
    int q0 = 1, q1 = globalQuantum;
    deque<int> q0dq, q1dq;
    vector<bool> arrived(process_count,false);
    int time=0, completed=0;
    while (completed < process_count) {
        for (int i=0;i<process_count;i++) if (!arrived[i] && processes[i].arrival <= time && remainingTime[i]>0) { q0dq.push_back(i); arrived[i]=true; }
        int cur=-1, tq=0;
        if (!q0dq.empty()) { cur = q0dq.front(); q0dq.pop_front(); tq=q0; }
        else if (!q1dq.empty()) { cur = q1dq.front(); q1dq.pop_front(); tq=q1; }
        else { ganttChart.push_back("Idle"); readyQueues.push_back(vector<string>()); time++; continue; }

        int run = min(tq, remainingTime[cur]);
        for (int t=time; t<time+run && t<last_instant; ++t) {
            for (int j=0;j<process_count;j++) if (!arrived[j] && processes[j].arrival <= t && remainingTime[j]>0) { q0dq.push_back(j); arrived[j]=true; }
            vector<string> snap;
            for (int id: q0dq) snap.push_back(processes[id].name);
            for (int id: q1dq) snap.push_back(processes[id].name);
            readyQueues.push_back(snap);

            timeline[t][cur] = '*';
            ganttChart.push_back(processes[cur].name);
            timelinePerProcess[cur][t] = '*';
            if (responseTime[cur]==-1) responseTime[cur] = t - processes[cur].arrival;
            remainingTime[cur]--;
        }
        time += run;
        if (remainingTime[cur] > 0) { q1dq.push_back(cur); }
        else { finishTime[cur] = time; completed++; }
    }
    fillInWaitMarkers();
    computeStats();
}

// 9) MLFQ exponential (kept as 'M' code in frontend)
void mlfq_exponential() {
    prepareRun();
    const int MAX_LEVELS = 8;
    vector<deque<int>> levels(MAX_LEVELS);
    vector<int> quantum(MAX_LEVELS,1);
    for (int l=1;l<MAX_LEVELS;l++) quantum[l] = quantum[l-1]*2;

    vector<bool> arrived(process_count,false);
    int time=0, completed=0;

    while (completed < process_count) {
        for (int i=0;i<process_count;i++) if (!arrived[i] && processes[i].arrival <= time && remainingTime[i]>0) { levels[0].push_back(i); arrived[i]=true; }
        int lev = -1;
        for (int l=0;l<MAX_LEVELS;l++) if (!levels[l].empty()) { lev=l; break; }
        if (lev==-1) { ganttChart.push_back("Idle"); readyQueues.push_back(vector<string>()); time++; continue; }
        int cur = levels[lev].front(); levels[lev].pop_front();
        int run = min(quantum[lev], remainingTime[cur]);
        for (int t=time; t<time+run && t<last_instant; ++t) {
            for (int j=0;j<process_count;j++) if (!arrived[j] && processes[j].arrival <= t && remainingTime[j]>0) { levels[0].push_back(j); arrived[j]=true; }
            vector<string> snap;
            for (int l=0;l<MAX_LEVELS;l++) for (int id: levels[l]) snap.push_back(processes[id].name);
            readyQueues.push_back(snap);

            timeline[t][cur] = '*';
            ganttChart.push_back(processes[cur].name);
            timelinePerProcess[cur][t] = '*';
            if (responseTime[cur]==-1) responseTime[cur] = t - processes[cur].arrival;
            remainingTime[cur]--;
        }
        time += run;
        if (remainingTime[cur] > 0) {
            int nxt = min(lev+1, MAX_LEVELS-1);
            levels[nxt].push_back(cur);
        } else {
            finishTime[cur] = time; completed++;
        }
    }
    fillInWaitMarkers();
    computeStats();
}

// 10) Aging
void aging_priority(int baseQuantum) {
    prepareRun();
    vector<int> curPriority(process_count);
    for (int i=0;i<process_count;i++) curPriority[i] = processes[i].priority;

    int time=0, completed=0;
    while (completed < process_count) {
        int best=-1;
        for (int i=0;i<process_count;i++) {
            if (remainingTime[i] > 0 && processes[i].arrival <= time) {
                if (best==-1 || higherPriority(curPriority[i], curPriority[best])) best = i;
            }
        }
        if (best==-1) { ganttChart.push_back("Idle"); readyQueues.push_back(vector<string>()); time++; continue; }

        int run = max(1, baseQuantum); run = min(run, remainingTime[best]);
        for (int t=time; t<time+run && t<last_instant; ++t) {
            vector<string> snap = buildReadySnapshot(t, best);
            readyQueues.push_back(snap);

            timeline[t][best] = '*';
            ganttChart.push_back(processes[best].name);
            timelinePerProcess[best][t] = '*';
            if (responseTime[best]==-1) responseTime[best] = t - processes[best].arrival;
            remainingTime[best]--;
        }
        time += run;
        if (remainingTime[best]==0) { finishTime[best] = time; completed++; }

        // Simple aging: bump priority of all waiting tasks toward "better"
        for (int i=0;i<process_count;i++) {
            if (remainingTime[i] > 0 && processes[i].arrival <= time && i != best) {
                if (priority_low_to_high) curPriority[i] = max(0, curPriority[i] - 1);
                else curPriority[i] = curPriority[i] + 1;
            }
        }
    }
    fillInWaitMarkers();
    computeStats();
}

// ---------------- Driver ----------------
void executeSelectedAlgorithm() {
    if (selectedAlgoId == '1') firstComeFirstServe();
    else if (selectedAlgoId == '3') shortestJobFirst_nonpreemptive();
    else if (selectedAlgoId == '4') srtf_preemptive();
    else if (selectedAlgoId == '2') roundRobin_timeSliced( (selectedAlgoQuantum > 0) ? selectedAlgoQuantum : globalQuantum );
    else if (selectedAlgoId == 'A' || selectedAlgoId == 'a') priority_preemptive();
    else if (selectedAlgoId == '9') priority_nonpreemptive();
    else if (selectedAlgoId == '7') hrrn_nonpreemptive();
    else if (selectedAlgoId == '8') mlfq_firstlevel();
    else if (selectedAlgoId == 'M') mlfq_exponential();
    else if (selectedAlgoId == 'L') aging_priority(globalQuantum);
    else firstComeFirstServe();
}

// ---------------- JSON emit ----------------
static string escape_json(const string &s) {
    string out; out.reserve(s.size()*2);
    for (char c: s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else out += c;
        }
    }
    return out;
}

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    parse_input();
    if (selectedAlgoId == '2' && selectedAlgoQuantum > 0) globalQuantum = selectedAlgoQuantum;
    executeSelectedAlgorithm();

    // ====== IMPORTANT TRIM STEP ======
    // We only export the actually used time series length.
    int usedTime = (int)ganttChart.size();

    // Align readyQueues length with used time
    if ((int)readyQueues.size() > usedTime) readyQueues.resize(usedTime);
    while ((int)readyQueues.size() < usedTime) readyQueues.push_back(vector<string>{});

    // Compact per-process rows to usedTime
    for (int i = 0; i < process_count; ++i) {
        string row = string(usedTime, ' ');
        for (int t = 0; t < usedTime && t < (int)timeline.size(); ++t) {
            if (i < (int)timeline[t].size()) row[t] = timeline[t][i];
        }
        timelinePerProcess[i] = row;
    }

    // Export last_instant as the REAL used time
    last_instant = usedTime;
    // ====== /TRIM ======

    // Emit JSON
    cout << "{\n";

    cout << "  \"gantt\": [";
    for (size_t i = 0; i < ganttChart.size(); ++i) {
        cout << "\"" << escape_json(ganttChart[i]) << "\"";
        if (i+1 < ganttChart.size()) cout << ", ";
    }
    cout << "],\n";

    cout << "  \"timeline\": [";
    for (int i=0;i<process_count;i++) {
        cout << "\"" << escape_json(timelinePerProcess[i]) << "\"";
        if (i+1<process_count) cout << ", ";
    }
    cout << "],\n";

    cout << "  \"readyQueues\": [";
    for (size_t t = 0; t < readyQueues.size(); ++t) {
        cout << "[";
        for (size_t j = 0; j < readyQueues[t].size(); ++j) {
            cout << "\"" << escape_json(readyQueues[t][j]) << "\"";
            if (j+1 < readyQueues[t].size()) cout << ", ";
        }
        cout << "]";
        if (t+1 < readyQueues.size()) cout << ", ";
    }
    cout << "],\n";

    cout << "  \"processes\": [";
    for (int i=0;i<process_count;i++) {
        cout << "{";
        cout << "\"name\":\"" << escape_json(processes[i].name) << "\",";
        cout << "\"arrival\":" << processes[i].arrival << ",";
        cout << "\"service\":" << processes[i].service << ",";
        cout << "\"priority\":" << processes[i].priority << ",";
        cout << "\"finish\":" << finishTime[i] << ",";
        cout << "\"tat\":" << turnAroundTime[i] << ",";
        cout << "\"normTurn\":" << fixed << setprecision(6) << normTurn[i] << ",";
        cout << "\"wait\":" << waitTime[i] << ",";
        cout << "\"resp\":" << responseTime[i];
        cout << "}";
        if (i+1<process_count) cout << ", ";
    }
    cout << "],\n";

    int cnt = process_count;
    double sT=0,sN=0,sW=0,sR=0;
    for (int i=0;i<cnt;i++) { sT += turnAroundTime[i]; sN += normTurn[i]; sW += waitTime[i]; sR += (responseTime[i]>=0?responseTime[i]:0); }

    cout << "  \"averages\": {";
    if (cnt>0) {
        cout << "\"tat\": " << fixed << setprecision(6) << (sT/cnt) << ", ";
        cout << "\"normTurn\": " << fixed << setprecision(6) << (sN/cnt) << ", ";
        cout << "\"wait\": " << fixed << setprecision(6) << (sW/cnt) << ", ";
        cout << "\"resp\": " << fixed << setprecision(6) << (sR/cnt);
    } else {
        cout << "\"tat\": 0, \"normTurn\": 0, \"wait\": 0, \"resp\": 0";
    }
    cout << "},\n";

    cout << "  \"last_instant\": " << last_instant << "\n";
    cout << "}\n";
    return 0;
}
