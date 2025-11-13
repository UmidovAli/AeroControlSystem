#include <bits/stdc++.h>
using namespace std;

// -------------------- CONFIG ------------------------
const double SIM_TIME = 1000.0;     // общая симуляция времени
const double ARRIVAL_RATE = 0.05;   // lambda для Пуассона
const double SERVICE_LOW = 5.0;
const double SERVICE_HIGH = 15.0;
const int NUM_SERVERS = 3;
const int BUFFER_SIZE = 10;
// ---------------------------------------------------

// -------------------- Random Generators -------------------
default_random_engine rng(42);
exponential_distribution<double> exp_dist(ARRIVAL_RATE);
uniform_real_distribution<double> uni_dist(SERVICE_LOW, SERVICE_HIGH);

double exponential(double rate) {
    return exp_dist(rng);
}

double uniform_service() {
    return uni_dist(rng);
}

// -------------------- Passenger ----------------------
struct Passenger {
    int id;
    double arrival_time;
    double service_time = 0.0;
    double start_service_time = -1.0;
    double end_service_time = -1.0;
    bool rejected = false;
};

// -------------------- Event --------------------------
enum EventType { ARRIVAL, SERVICE_COMPLETE };

struct Event {
    double time;
    int priority;
    EventType type;
    int server_id;        // нужен для SERVICE_COMPLETE
    Passenger* passenger; // нужен для ARRIVAL
    bool operator<(const Event& other) const {
        return time > other.time; // min-heap
    }
};

// -------------------- Circular Buffer -----------------
class QueueBuffer {
    int capacity;
    vector<Passenger*> buffer;
    vector<int> index_order; // порядок поступления
    int head = 0;
    int tail = 0;
    int size = 0;
    int circular_pointer = 0;
public:
    QueueBuffer(int cap) : capacity(cap), buffer(cap, nullptr) {}

    bool is_full() { return size >= capacity; }
    int len() { return size; }

    void enqueue(Passenger* p) {
        if(is_full()) throw runtime_error("enqueue on full buffer");
        buffer[tail] = p;
        index_order.push_back(tail);
        tail = (tail + 1) % capacity;
        size++;
    }

    Passenger* remove_oldest() {
        if(size == 0) throw runtime_error("remove_oldest on empty buffer");
        int oldest_index = index_order.front();
        index_order.erase(index_order.begin());
        Passenger* p = buffer[oldest_index];
        buffer[oldest_index] = nullptr;
        size--;
        while(size > 0 && buffer[head] == nullptr)
            head = (head + 1) % capacity;
        return p;
    }

    Passenger* dequeue_next_circular() {
        if(size == 0) throw runtime_error("dequeue on empty buffer");
        int attempts = 0;
        while(attempts < capacity) {
            int idx = circular_pointer % capacity;
            if(buffer[idx] != nullptr) {
                Passenger* p = buffer[idx];
                buffer[idx] = nullptr;
                index_order.erase(remove(index_order.begin(), index_order.end(), idx), index_order.end());
                circular_pointer = (idx + 1) % capacity;
                size--;
                while(size > 0 && buffer[head] == nullptr)
                    head = (head + 1) % capacity;
                return p;
            } else {
                circular_pointer = (circular_pointer + 1) % capacity;
                attempts++;
            }
        }
        throw runtime_error("circular dequeue failed");
    }
};

// -------------------- Server -------------------------
struct Server {
    int id;
    bool busy = false;
    Passenger* current = nullptr;
    vector<pair<double,double>> timeline; // busy intervals

    void serve(Passenger* p, double now) {
        busy = true;
        current = p;
        p->start_service_time = now;
    }

    void release(double now) {
        if(current) {
            current->end_service_time = now;
            timeline.push_back({current->start_service_time, current->end_service_time});
            current = nullptr;
        }
        busy = false;
    }
};

// -------------------- Statistics ----------------------
struct Statistics {
    vector<Passenger*> served;
    vector<Passenger*> rejected;
    vector<double> wait_times;
    vector<pair<double,int>> queue_history;

    void record_enqueue(double t, int qlen) {
        queue_history.push_back({t, qlen});
    }

    void record_service(Passenger* p) {
        served.push_back(p);
        wait_times.push_back(p->start_service_time - p->arrival_time);
    }

    void record_rejection(Passenger* p) {
        p->rejected = true;
        rejected.push_back(p);
    }

    void print_summary(vector<Server>& servers, double sim_time) {
        double avg_wait = wait_times.empty() ? 0.0 : accumulate(wait_times.begin(), wait_times.end(), 0.0)/wait_times.size();
        vector<double> utilizations;
        for(auto& s: servers) {
            double busy_time = 0;
            for(auto& iv: s.timeline) busy_time += iv.second - iv.first;
            utilizations.push_back(busy_time / sim_time);
        }
        double avg_util = utilizations.empty() ? 0.0 : accumulate(utilizations.begin(), utilizations.end(), 0.0)/utilizations.size();
        double avg_queue_len = queue_history.empty() ? 0.0 : accumulate(queue_history.begin(), queue_history.end(), 0.0,
            [](double sum, pair<double,int> p){return sum + p.second;}) / queue_history.size();

        cout << "Served: " << served.size() << "\n";
        cout << "Rejected: " << rejected.size() << "\n";
        cout << "Average wait: " << avg_wait << "\n";
        cout << "Average server utilization: " << avg_util << "\n";
        cout << "Average queue length: " << avg_queue_len << "\n";
        for(int i=0;i<servers.size();i++)
            cout << "Server " << servers[i].id << " utilization: " << utilizations[i] << "\n";
    }
};

// -------------------- Inspection System -------------------
class InspectionSystem {
    double now = 0.0;
    int passenger_counter = 0;
    double next_arrival_time = 0.0;

    QueueBuffer buffer;
    vector<Server> servers;
    Statistics stats;
    priority_queue<Event> event_queue;

public:
    InspectionSystem() : buffer(BUFFER_SIZE), servers(NUM_SERVERS) {
        for(int i=0;i<NUM_SERVERS;i++) servers[i].id = i+1;
    }

    void schedule_event(double t, int priority, EventType type, int server_id, Passenger* p) {
        event_queue.push({t, priority, type, server_id, p});
    }

    Server* find_free_server_lowest() {
        for(auto& s: servers)
            if(!s.busy) return &s;
        return nullptr;
    }

    void start() {
        // первая прибытие
        next_arrival_time = exponential(ARRIVAL_RATE);
        schedule_event(next_arrival_time, 1, ARRIVAL, -1, nullptr);

        while(!event_queue.empty()) {
            Event ev = event_queue.top(); event_queue.pop();
            if(ev.time > SIM_TIME) break;
            now = ev.time;

            if(ev.type == ARRIVAL) handle_arrival();
            else if(ev.type == SERVICE_COMPLETE) handle_service_complete(ev.server_id);
        }

        stats.print_summary(servers, SIM_TIME);
    }

    void handle_arrival() {
        passenger_counter++;
        Passenger* p = new Passenger{passenger_counter, now};

        // планируем следующее прибытие
        double ia = exponential(ARRIVAL_RATE);
        next_arrival_time = now + ia;
        schedule_event(next_arrival_time, 1, ARRIVAL, -1, nullptr);

        // ищем свободный сервер
        Server* free = find_free_server_lowest();
        if(free) {
            p->service_time = uniform_service();
            free->serve(p, now);
            schedule_event(now + p->service_time, 2, SERVICE_COMPLETE, free->id, nullptr);
            stats.record_service(p);
        } else {
            if(!buffer.is_full()) {
                buffer.enqueue(p);
                stats.record_enqueue(now, buffer.len());
            } else {
                Passenger* oldest = buffer.remove_oldest();
                stats.record_rejection(oldest);
                buffer.enqueue(p);
                stats.record_enqueue(now, buffer.len());
            }
        }
    }

    void handle_service_complete(int server_id) {
        Server& s = servers[server_id-1];
        s.release(now);

        if(buffer.len() > 0) {
            Passenger* next_p = buffer.dequeue_next_circular();
            next_p->service_time = uniform_service();
            s.serve(next_p, now);
            schedule_event(now + next_p->service_time, 2, SERVICE_COMPLETE, s.id, nullptr);
            stats.record_service(next_p);
            stats.record_enqueue(now, buffer.len());
        }
    }
};

// -------------------- Main ----------------------------
int main() {
    InspectionSystem sim;
    sim.start();
    return 0;
}
