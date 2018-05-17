/*
    Karabo bridge client.

    Copyright (c) 2018, European X-Ray Free-Electron Laser Facility GmbH
    All rights reserved.

    You should have received a copy of the 3-Clause BSD License along with this
    program. If not, see <https://opensource.org/licenses/BSD-3-Clause>
*/

#ifndef KARABO_BRIDGE_CPP_KB_CLIENT_HPP
#define KARABO_BRIDGE_CPP_KB_CLIENT_HPP

#include <zmq.hpp>
#include <msgpack.hpp>

#include <string>
#include <stack>
#include <array>
#include <deque>
#include <iostream>
#include <sstream>
#include <fstream>
#include <exception>
#include <limits>


namespace karabo_bridge {

/*
 * A proxy object for deferred unpack.
 *
 * typedef enum {
        MSGPACK_OBJECT_NIL                  = 0x00,
        MSGPACK_OBJECT_BOOLEAN              = 0x01,
        MSGPACK_OBJECT_POSITIVE_INTEGER     = 0x02,
        MSGPACK_OBJECT_NEGATIVE_INTEGER     = 0x03,
        MSGPACK_OBJECT_FLOAT32              = 0x0a,
        MSGPACK_OBJECT_FLOAT64              = 0x04,
        MSGPACK_OBJECT_FLOAT                = 0x04,
        MSGPACK_OBJECT_STR                  = 0x05,
        MSGPACK_OBJECT_ARRAY                = 0x06,
        MSGPACK_OBJECT_MAP                  = 0x07,
        MSGPACK_OBJECT_BIN                  = 0x08,
        MSGPACK_OBJECT_EXT                  = 0x09
   } msgpack_object_type;
 */
struct object {
    object() = default;  // must be default constructable
    explicit object(const msgpack::object& value): value_(value) {}

    // copy is not allowed
    object(const object&) = delete;
    object& operator=(const object&) = delete;

    object(object&&) noexcept = default;
    object& operator=(object&&) noexcept = default;
    /*
     * Cast the held msgpack::object to a given type.
     *
     * Exceptions:
     * std::bad_cast if the cast fails.
     */
    template<typename T>
    T as() { return value_.as<T>(); }

    /*
     * Cast the held msgpack::object::BIN object to a 1D std::array.
     *
     * Parameters:
     * N: size of the output std::array.
     *
     * Note: users are responsible to give the correct data type and size,
     *       otherwise it leads to undefined behavior.
     *
     * Exceptions:
     * std::bad_cast if the cast fails.
     */
    template<typename T, std::size_t N>
    std::array<T, N> asArray() {
        std::array<T, N> result;

        auto tmp = value_.as<std::array<char, sizeof(T)*N>>();
        std::memcpy(&result, &tmp, sizeof(T)*N);
        return result;
    }

    msgpack::object get() const { return value_; }

    uint16_t type() const { return value_.type; }

private:
    msgpack::object value_;
};

} // karabo_bridge


namespace msgpack {
MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS) {
namespace adaptor{

/*
 * template specialization for karabo_bridge::object
 */
template<>
struct as<karabo_bridge::object> {
    karabo_bridge::object operator()(msgpack::object const& o) const {
        return karabo_bridge::object(o.as<msgpack::object>());
    }
};

} // adaptor
} // MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS)
} // msgpack


namespace karabo_bridge {

/*
 * Visitor used to unfold the hierarchy of an unknown data structure,
 */
struct karabo_visitor {
    std::string& m_s;
    bool m_ref;

    explicit karabo_visitor(std::string& s):m_s(s), m_ref(false) {}
    ~karabo_visitor() { m_s += "\n"; }

    bool visit_nil() {
        m_s += "null";
        return true;
    }
    bool visit_boolean(bool v) {
        if (v) m_s += "true";
        else m_s += "false";
        return true;
    }
    bool visit_positive_integer(uint64_t v) {
        std::stringstream ss;
        ss << v;
        m_s += ss.str();
        return true;
    }
    bool visit_negative_integer(int64_t v) {
        std::stringstream ss;
        ss << v;
        m_s += ss.str();
        return true;
    }
    bool visit_float32(float v) {
        std::stringstream ss;
        ss << v;
        m_s += ss.str();
        return true;
    }
    bool visit_float64(double v) {
        std::stringstream ss;
        ss << v;
        m_s += ss.str();
        return true;
    }
    bool visit_str(const char* v, uint32_t size) {
        m_s += '"' + std::string(v, size) + '"';
        return true;
    }
    bool visit_bin(const char* v, uint32_t size) {
        if (is_key_)
            m_s += std::string(v, size);
        else m_s += "(bin)";
        return true;
    }
    bool visit_ext(const char* /*v*/, uint32_t /*size*/) {
        return true;
    }
    bool start_array_item() {
        return true;
    }
    bool start_array(uint32_t size) {
        m_s += "[";
        return true;
    }
    bool end_array_item() {
        m_s += ",";
        return true;
    }
    bool end_array() {
        m_s.erase(m_s.size() - 1, 1); // remove the last ','
        m_s += "]";
        return true;
    }
    bool start_map(uint32_t /*num_kv_pairs*/) {
        tracker_.push(level_++);
        return true;
    }
    bool start_map_key() {
        is_key_ = true;
        m_s += "\n";

        for (int i=0; i< tracker_.top(); ++i) m_s += "    ";
        return true;
    }
    bool end_map_key() {
        m_s += ": ";
        is_key_ = false;
        return true;
    }
    bool start_map_value() {
        return true;
    }
    bool end_map_value() {
        m_s += ",";
        return true;
    }
    bool end_map() {
        m_s.erase(m_s.size() - 1, 1); // remove the last ','
        tracker_.pop();
        --level_;
        return true;
    }
    void parse_error(size_t /*parsed_offset*/, size_t /*error_offset*/) {
        std::cerr << "parse error"<<std::endl;
    }
    void insufficient_bytes(size_t /*parsed_offset*/, size_t /*error_offset*/) {
        std::cout << "insufficient bytes"<<std::endl;
    }

    // These two functions are required by parser.
    void set_referenced(bool ref) { m_ref = ref; }
    bool referenced() const { return m_ref; }

private:
    std::stack<int> tracker_;
    uint16_t level_ = 0;
    bool is_key_ = false;
};

/*
 * Helper function: convert a message to string.
 */
inline void msg2str(zmq::message_t& msg, std::string& str) {
    str = std::string(static_cast<const char *>(msg.data()), msg.size());
}

/*
 * Data structure presented to the user.
 */
struct data {
    std::map<std::string, object> metadata;
    std::map<std::string, object> data_;

    /*
     * Exceptions:
     * std::out_of_range if key is invalid.
     */
    object& operator[](std::string key) {
        return data_.at(key);
    }
};

using multipart_msg = std::deque<zmq::message_t>;

/*
 * Parse a single message packed by msgpack using "visitor".
 */
std::string parseMsg(const zmq::message_t& msg) {
    std::string data_str;
    karabo_visitor visitor(data_str);
    bool ret = msgpack::parse(static_cast<const char*>(msg.data()), msg.size(), visitor);
    assert(ret);
    return data_str;
}

/*
 * Parse a multipart message packed by msgpack using "visitor".
 */
std::string parseMultipartMsg(const multipart_msg& mpmsg, bool boundary=true) {
    std::string output;
    std::string separator("\n----------new message----------\n");
    for (auto& msg : mpmsg) {
        if (boundary) output.append(separator);
        output.append(parseMsg(msg));
    }
    return output;
}


/*
 * Karabo-bridge Client class.
 */
class Client {
    zmq::context_t ctx_;
    zmq::socket_t socket_;

    /*
     * Send a "next" request to server.
     */
    void sendRequest() {
        zmq::message_t request(4);
        memcpy(request.data(), "next", request.size());
        socket_.send(request);
    }

    /*
     * Receive a multipart message from the server.
     */
    multipart_msg receiveMultipartMsg() {
        int64_t more;  // multipart checker
        multipart_msg mpmsg;
        while (true) {
            zmq::message_t msg;
            socket_.recv(&msg);
            mpmsg.emplace_back(std::move(msg));
            std::size_t more_size = sizeof(int64_t);
            socket_.getsockopt(ZMQ_RCVMORE, &more, &more_size);
            if (more == 0) break;
        }
        return mpmsg;
    }

public:
    Client(): ctx_(1), socket_(ctx_, ZMQ_REQ) {}

    void connect(const std::string& endpoint) {
        std::cout << "Connecting to server: " << endpoint << std::endl;
        socket_.connect(endpoint.c_str());
    }

    /*
     * Request and return the next data from the server.
     */
    data next() {
        using MsgObjectMap = std::map<std::string, msgpack::object>;
        data kbdt;

        sendRequest();
        multipart_msg mpmsg = receiveMultipartMsg();
        if (mpmsg.empty()) return kbdt;

        // deal with the first message
        msgpack::object_handle oh_root;
        msgpack::unpack(oh_root, static_cast<const char*>(mpmsg[0].data()), mpmsg[0].size());
        auto root_unpacked = oh_root.get().as<MsgObjectMap>();
        std::string source = root_unpacked.at("source").as<std::string>();

        if (auto dt_content = root_unpacked.at("content").as<std::string>() != "msgpack")
            throw std::runtime_error("Unknown data content!" + dt_content);

        for (auto it = mpmsg.begin() + 1; it != mpmsg.end(); ++it) {
            msgpack::object_handle oh;
            msgpack::unpack(oh, static_cast<const char*>(it->data()), it->size());
            auto data_unpacked = oh.get().as<MsgObjectMap>();

            if (data_unpacked.find("content") != data_unpacked.end()) {
                if (data_unpacked.at("source").as<std::string>() != source)
                    throw std::runtime_error("Inconsistent data source!");

                auto content = data_unpacked["content"].as<std::string>();
                if (content == "array") {
                    std::advance(it, 1);
                    msgpack::zone zone;
                    msgpack::object obj(msgpack::type::raw_ref(static_cast<const char*>(it->data()),
                                                               it->size()),
                                        zone);
                    kbdt.data_.insert(std::make_pair(data_unpacked.at("path").as<std::string>(),
                                                     obj.as<object>()));
                } else if (content == "ImageData") {
                    throw std::runtime_error("ImageData unimplemented: " + content);
                }
                else
                    throw std::runtime_error("Unknown data content: " + content);

            } else {
                for (auto &dt : data_unpacked) {
                    if (dt.first == "metadata") { // "metadata" has another level of dictionary
                        auto unpacked_timestamp = dt.second.as<MsgObjectMap>();
                        for (auto &v : unpacked_timestamp)
                            kbdt.metadata.insert(std::make_pair(v.first, v.second.as<object>()));
                    } else // normal data represented by the key-value pair
                        kbdt.data_.insert(std::make_pair(dt.first, dt.second.as<object>()));
                }
            }
        }

        return kbdt;
    }

    /*
     * Parse the structure of the next coming data and save it to a file.
     *
     * Note:: this function consumes data!!!
     */
    void showNext(const std::string& fname="data_structure_from_server.txt") {
        sendRequest();
        auto mpmsg = receiveMultipartMsg();

        std::ofstream out(fname);
        out << parseMultipartMsg(mpmsg);
        out.close();
    }
};

} // karabo_bridge

#endif //KARABO_BRIDGE_CPP_KB_CLIENT_HPP