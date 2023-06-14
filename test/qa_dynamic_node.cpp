#include <graph.hpp>

#include <list>

namespace fg = fair::graph;

template<typename T>
class multi_adder : public fg::node_model {
protected:
    using in_port_t = fg::IN<T>;
    // std::list because ports don't like to change in-memory address
    // after connection is established, and vector might reallocate
    std::list<in_port_t> _input_ports;
    fg::OUT<T>           _output_port;

public:
    multi_adder(std::size_t input_ports_size) {
        _input_ports.resize(input_ports_size);
        for (auto &input_port : _input_ports) {
            _dynamic_input_ports.emplace_back(input_port);
        }
        _dynamic_output_ports.emplace_back(_output_port);
        _dynamic_ports_loaded = true;
    };

    ~multi_adder() override = default;

    std::string_view
    name() const override {
        return "";
    }

    virtual fg::work_return_t
    work() override {
        // TODO: Rewrite with ranges once we can use them
        std::size_t available_samples = -1;
        for (const auto &input_port : _input_ports) {
            auto available_samples_for_port = input_port.streamReader().available();
            if (available_samples_for_port < available_samples) {
                available_samples = available_samples_for_port;
            }
        }

        if (available_samples == 0) {
            return fg::work_return_t::OK;
        }

        std::vector<std::span<const double>> readers;
        for (auto &input_port : _input_ports) {
            readers.push_back(input_port.streamReader().get(available_samples));
        }

        auto &writer = _output_port.streamWriter();
        writer.publish(
                [available_samples, &readers](std::span<T> output) {
                    // const auto input = reader.get(n_to_publish);
                    for (std::size_t i = 0; i < available_samples; ++i) {
                        output[i] = std::accumulate(readers.cbegin(), readers.cend(), 0, [i](T sum, auto span) { return sum + span[i]; });
                    }
                },
                available_samples);

        for (auto &input_port : _input_ports) {
            assert(available_samples == input_port.streamReader().consume(available_samples));
        }
        return fg::work_return_t::OK;
    }

    virtual void *
    raw() override {
        return this;
    }

    void
    add_new_input_port() {
        _input_ports.push_back({});
        _dynamic_input_ports.emplace_back(_input_ports.back());
    }
};

template<typename T>
class fixed_source : public fg::node<fixed_source<T>, fg::OUT<T, 0, 1024, "out">> {
public:
    fixed_source() {}

    T value = 1;

    fg::work_return_t
    work() {
        using namespace fair::literals;
        auto &port   = fg::output_port<0>(this);
        auto &writer = port.streamWriter();
        auto  data   = writer.reserve_output_range(1_UZ);
        data[0]      = value;
        data.publish(1_UZ);

        value += 1;
        return fg::work_return_t::OK;
    }
};

template<typename T>
class cout_sink : public fg::node<cout_sink<T>, fg::IN<T, 0, 1024, "in">> {
    std::size_t _remaining = 0;

public:
    cout_sink() {}

    explicit cout_sink(std::size_t count) : _remaining(count) {}

    void
    process_one(T value) {
        _remaining--;
        if (_remaining == 0) {
            std::cerr << "last value was: " << value << "\n";
        }
    }
};

int
main() {
    constexpr const std::size_t sources_count = 10;
    constexpr const std::size_t events_count  = 5;

    fg::graph                   flow_graph;

    // Adder has sources_count inputs in total, but let's create
    // sources_count - 1 inputs on construction, and add an additional one
    // later
    auto &adder = flow_graph.add_node(std::make_unique<multi_adder<double>>(sources_count - 1));
    auto &sink  = flow_graph.make_node<cout_sink<double>>(events_count);

    //
    std::vector<fixed_source<double> *> sources;

    // Function that adds a new source node to the graph, and connects
    // it to one of adder's ports
    auto add_source = [&] {
        auto &source = flow_graph.make_node<fixed_source<double>>();
        sources.push_back(&source);
        flow_graph.dynamic_connect(source, 0, adder, sources.size() - 1);
    };

    for (std::size_t i = 0; i < sources_count - 1; ++i) {
        add_source();
    }

    flow_graph.dynamic_connect(adder, 0, sink, 0);

    static_cast<multi_adder<double> &>(adder).add_new_input_port();
    add_source();

    for (std::size_t i = 0; i < events_count; ++i) {
        for (auto *source : sources) {
            source->work();
        }
        adder.work();
        sink.work();
    }
}
