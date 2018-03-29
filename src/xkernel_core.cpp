/***************************************************************************
* Copyright (c) 2016, Johan Mabille and Sylvain Corlay                     *
*                                                                          *
* Distributed under the terms of the BSD 3-Clause License.                 *
*                                                                          *
* The full license is in the file LICENSE, distributed with this software. *
****************************************************************************/

#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <tuple>

#include "xkernel_core.hpp"

using namespace std::placeholders;

namespace xeus
{

    xkernel_core::xkernel_core(const std::string& kernel_id,
                               const std::string& user_name,
                               const std::string& session_id,
                               authentication_ptr auth,
                               server_ptr server,
                               interpreter_ptr interpreter)
        : m_kernel_id(std::move(kernel_id)),
          m_user_name(std::move(user_name)),
          m_session_id(std::move(session_id)),
          p_auth(std::move(auth)),
          m_comm_manager(this),
          p_server(server),
          p_interpreter(interpreter),
          m_parent_id(0),
          m_parent_header(xjson::object())
    {
        // Request handlers
        m_handler["execute_request"] = &xkernel_core::execute_request;
        m_handler["complete_request"] = &xkernel_core::complete_request;
        m_handler["inspect_request"] = &xkernel_core::inspect_request;
        m_handler["history_request"] = &xkernel_core::history_request;
        m_handler["is_complete_request"] = &xkernel_core::is_complete_request;
        m_handler["comm_info_request"] = &xkernel_core::comm_info_request;
        m_handler["comm_open"] = &xkernel_core::comm_open;
        m_handler["comm_close"] = &xkernel_core::comm_close;
        m_handler["comm_msg"] = &xkernel_core::comm_msg;
        m_handler["kernel_info_request"] = &xkernel_core::kernel_info_request;
        m_handler["shutdown_request"] = &xkernel_core::shutdown_request;
        m_handler["interrupt_request"] = &xkernel_core::interrupt_request;

        // Server bindings
        p_server->register_shell_listener(std::bind(&xkernel_core::dispatch_shell, this, _1));
        p_server->register_control_listener(std::bind(&xkernel_core::dispatch_control, this, _1));
        p_server->register_stdin_listener(std::bind(&xkernel_core::dispatch_stdin, this, _1));

        // Interpreter bindings
        p_interpreter->register_publisher(std::bind(&xkernel_core::publish_message, this, _1, _2, _3));
        p_interpreter->register_stdin_sender(std::bind(&xkernel_core::send_stdin, this, _1, _2, _3));
        p_interpreter->register_comm_manager(&m_comm_manager);
    }

    void xkernel_core::dispatch_shell(zmq::multipart_t& wire_msg)
    {
        dispatch(wire_msg, channel::SHELL);
    }

    void xkernel_core::dispatch_control(zmq::multipart_t& wire_msg)
    {
        dispatch(wire_msg, channel::CONTROL);
    }

    void xkernel_core::dispatch_stdin(zmq::multipart_t& wire_msg)
    {
        xmessage msg;
        try
        {
            msg.deserialize(wire_msg, *p_auth);
        }
        catch (std::exception& e)
        {
            std::cerr << "ERROR: could not deserialize message" << std::endl;
            std::cerr << e.what() << std::endl;
            return;
        }

        const xjson& header = msg.header();
        std::string msg_type = header.value("msg_type", "");
    }

    void xkernel_core::publish_message(const std::string& msg_type,
                                       xjson metadata,
                                       xjson content)
    {
        xpub_message msg(get_topic(msg_type),
                         make_header(msg_type, m_user_name, m_session_id),
                         get_parent_header(),
                         std::move(metadata),
                         std::move(content));
        zmq::multipart_t wire_msg;
        msg.serialize(wire_msg, *p_auth);
        p_server->publish(wire_msg);
    }

    void xkernel_core::send_stdin(const std::string& msg_type,
                                  xjson metadata,
                                  xjson content)
    {
        xmessage msg(get_parent_id(),
                     make_header(msg_type, m_user_name, m_session_id),
                     get_parent_header(),
                     std::move(metadata),
                     std::move(content));
        zmq::multipart_t wire_msg;
        msg.serialize(wire_msg, *p_auth);
        p_server->send_stdin(wire_msg);
    }

    xcomm_manager& xkernel_core::comm_manager() & noexcept
    {
        return m_comm_manager;
    }

    const xcomm_manager& xkernel_core::comm_manager() const & noexcept
    {
        return m_comm_manager;
    }

    xcomm_manager xkernel_core::comm_manager() const && noexcept
    {
        return m_comm_manager;
    }

    void xkernel_core::dispatch(zmq::multipart_t& wire_msg, channel c)
    {
        xmessage msg;
        try
        {
            msg.deserialize(wire_msg, *p_auth);
        }
        catch (std::exception& e)
        {
            std::cerr << "ERROR: could not deserialize message" << std::endl;
            std::cerr << e.what() << std::endl;
            return;
        }

        const xjson& header = msg.header();
        set_parent(msg.identities(), header);
        publish_status("busy");

        std::string msg_type = header.value("msg_type", "");
        handler_type handler = get_handler(msg_type);
        if (handler == nullptr)
        {
            std::cerr << "ERROR: received unknown message" << std::endl;
        }
        else
        {
            try
            {
                (this->*handler)(msg, c);
            }
            catch (std::exception& e)
            {
                std::cerr << "ERROR: received bad message: " << e.what() << std::endl;
                std::cerr << "Message content: " << msg.content() << std::endl;
            }
        }

        publish_status("idle");
    }

    auto xkernel_core::get_handler(const std::string& msg_type) -> handler_type
    {
        auto iter = m_handler.find(msg_type);
        handler_type res = (iter == m_handler.end()) ? nullptr : iter->second;
        return res;
    }

    void xkernel_core::interrupt_request(const xmessage& request, channel c) {
      p_interpreter->interrupt_request();
      send_reply("interrupt_reply", xjson::object(), std::move(xjson::object()), c);
    }

    void xkernel_core::execute_request(const xmessage& request, channel c)
    {
        try
        {
            const xjson& content = request.content();
            std::string code = content.value("code", "");
            bool silent = content.value("silent", false);
            bool store_history = content.value("store_history", true);
            store_history = store_history && !silent;
            const xjson_node* user_expression = get_json_node(content, "user_expressions");
            bool allow_stdin = content.value("allow_stdin", true);
            bool stop_on_error = content.value("stop_on_error", false);

            xjson metadata = get_metadata();

            xjson reply = p_interpreter->execute_request(code,
                                                         silent,
                                                         store_history,
                                                         user_expression,
                                                         allow_stdin);

            std::string status = reply.value("status", "error");
            send_reply("execute_reply", std::move(metadata), std::move(reply), c);

            if (!silent && status == "error" && stop_on_error)
            {
                long polling_interval = 50;
                p_server->abort_queue(std::bind(&xkernel_core::abort_request, this, _1), 50);
            }
        }
        catch (std::exception& e)
        {
            std::cerr << "ERROR: during execute_request" << std::endl;
            std::cerr << e.what() << std::endl;
        }
    }

    void xkernel_core::complete_request(const xmessage& request, channel c)
    {
        const xjson& content = request.content();
        std::string code = content.value("code", "");
        int cursor_pos = content.value("cursor_pos", -1);

        xjson reply = p_interpreter->complete_request(code, cursor_pos);
        send_reply("complete_request", xjson::object(), std::move(reply), c);
    }

    void xkernel_core::inspect_request(const xmessage& request, channel c)
    {
        const xjson& content = request.content();
        std::string code = content.value("code", "");
        int cursor_pos = content.value("cursor_pos", -1);
        int detail_level = content.value("detail_level", 0);

        xjson reply = p_interpreter->inspect_request(code, cursor_pos, detail_level);
        send_reply("inspect_reply", xjson::object(), std::move(reply), c);
    }

    void xkernel_core::history_request(const xmessage& request, channel c)
    {
        const xjson& content = request.content();
        xhistory_arguments args;
        args.m_hist_access_type = content.value("hist_access_type", "tail");
        args.m_output = content.value("output", false);
        args.m_raw = content.value("raw", false);
        args.m_session = content.value("session", 0);
        args.m_start = content.value("start", 0);
        args.m_stop = content.value("stop", 0);
        args.m_n = content.value("n", 0);
        args.m_pattern = content.value("pattern", "");
        args.m_unique = content.value("unique", false);

        xjson reply = p_interpreter->history_request(args);
        send_reply("history_reply", xjson::object(), std::move(reply), c);
    }

    void xkernel_core::is_complete_request(const xmessage& request, channel c)
    {
        const xjson& content = request.content();
        std::string code = content.value("code", "");

        xjson reply = p_interpreter->is_complete_request(code);
        send_reply("is_complete_reply", xjson::object(), std::move(reply), c);
    }

    void xkernel_core::comm_info_request(const xmessage& request, channel c)
    {
        const xjson& content = request.content();
        std::string target_name = content.is_null() ? "" : content.value("target_name", "");
        auto comms = xjson::object();
        for (auto it = m_comm_manager.comms().cbegin(); it != m_comm_manager.comms().cend(); ++it)
        {
            const std::string& name = it->second->target().name();
            if (target_name.empty() || name == target_name)
            {
                xjson info;
                info["target_name"] = name;
                comms[it->first] = std::move(info);
            }
        }
        xjson reply;
        reply["comms"] = comms;
        reply["status"] = "ok";
        send_reply("comm_info_reply", xjson::object(), std::move(reply), c);
    }

    void xkernel_core::kernel_info_request(const xmessage& /* request */, channel c)
    {
        xjson reply = p_interpreter->kernel_info_request();
        reply["protocol_version"] = get_protocol_version();
        send_reply("kernel_info_reply", xjson::object(), std::move(reply), c);
    }

    void xkernel_core::shutdown_request(const xmessage& request, channel c)
    {
        const xjson& content = request.content();
        bool restart = content.value("restart", false);
        p_server->stop();
        xjson reply;
        reply["restart"] = restart;
        publish_message("shutdown", xjson::object(), xjson(reply));
        send_reply("shutdown_reply", xjson::object(), std::move(reply), c);
    }

    void xkernel_core::publish_status(const std::string& status)
    {
        xjson content;
        content["execution_state"] = status;
        publish_message("status", xjson::object(), std::move(content));
    }

    void xkernel_core::publish_execute_input(const std::string& code,
                                             int execution_count)
    {
        xjson content;
        content["code"] = code;
        content["execution_count"] = execution_count;
        publish_message("execute_input", xjson::object(), std::move(content));
    }

    void xkernel_core::send_reply(const std::string& reply_type,
                                  xjson metadata,
                                  xjson reply_content,
                                  channel c)
    {
        send_reply(get_parent_id(),
                   reply_type,
                   get_parent_header(),
                   std::move(metadata),
                   std::move(reply_content),
                   c);
    }

    void xkernel_core::send_reply(const guid_list& id_list,
                                  const std::string& reply_type,
                                  xjson parent_header,
                                  xjson metadata,
                                  xjson reply_content,
                                  channel c)
    {
        xmessage reply(id_list,
                       make_header(reply_type, m_user_name, m_session_id),
                       std::move(parent_header),
                       std::move(metadata),
                       std::move(reply_content));
        zmq::multipart_t wire_msg;
        reply.serialize(wire_msg, *p_auth);
        if (c == channel::SHELL)
        {
            p_server->send_shell(wire_msg);
        }
        else
        {
            p_server->send_control(wire_msg);
        }
    }

    void xkernel_core::abort_request(zmq::multipart_t& wire_msg)
    {
        xmessage msg;
        try
        {
            msg.deserialize(wire_msg, *p_auth);
        }
        catch (std::exception& e)
        {
            std::cerr << "ERROR: during execute_request: " << e.what() << std::endl;
            return;
        }
        const xjson& header = msg.header();
        std::string msg_type = header.value("msg_type", "");
        // replace "_request" part of message type by "_reply"
        msg_type.replace(msg_type.find_last_of('_'), 8, "_reply");
        xjson content;
        content["status"] = "error";
        send_reply(msg.identities(),
                   msg_type,
                   xjson(header),
                   xjson::object(),
                   std::move(content),
                   channel::SHELL);
    }

    std::string xkernel_core::get_topic(const std::string& msg_type) const
    {
        return "kernel_core." + m_kernel_id + "." + msg_type;
    }

    xjson xkernel_core::get_metadata() const
    {
        xjson metadata;
        metadata["started"] = iso8601_now();
        return metadata;
    }

    void xkernel_core::set_parent(const guid_list& parent_id,
                                  const xjson& parent_header)
    {
        m_parent_id = parent_id;
        m_parent_header = xjson(parent_header);
    }

    const xkernel_core::guid_list& xkernel_core::get_parent_id() const
    {
        return m_parent_id;
    }

    xjson xkernel_core::get_parent_header() const
    {
        return m_parent_header;
    }

    void xkernel_core::comm_open(const xmessage& request, channel)
    {
        return m_comm_manager.comm_open(request);
    }

    void xkernel_core::comm_close(const xmessage& request, channel)
    {
        return m_comm_manager.comm_close(request);
    }

    void xkernel_core::comm_msg(const xmessage& request, channel)
    {
        return m_comm_manager.comm_msg(request);
    }
}
