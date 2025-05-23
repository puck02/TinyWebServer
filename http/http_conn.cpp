#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1; // Enable CGI flag for POST requests
    }
    else
        return BAD_REQUEST;

    // Skip leading spaces in URL
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    // Handle "http://" and "https://" prefixes
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    else if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;

    // When url is "/", serve index.html
    // Check if the URL is exactly "/"
    if (strcmp(m_url, "/") == 0) {
        // Overwrite "/" with "/index.html"
        // Ensure buffer has space. Assuming m_url points within m_read_buf.
        // Need space for "/index.html" + null terminator (12 chars)
        // Calculate remaining space from m_url's position
        size_t remaining_space = READ_BUFFER_SIZE - (m_url - m_read_buf);
        if (remaining_space > strlen("index.html")) { // Need space for "index.html" after the '/'
             strcpy(m_url + 1, "index.html"); // Overwrite starting from the character after '/'
        } else {
             LOG_ERROR("Buffer overflow prevented in parse_request_line for /index.html");
             return BAD_REQUEST; // Not enough space in buffer
        }
    }

    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    // Copy the document root to the beginning of m_real_file
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    const char *p = strrchr(m_url, '/'); // Find the last '/' in the URL

    // 自动补全静态页面的.html后缀
    if (strcmp(m_url, "/login") == 0)
        strcpy(m_url, "/login.html");
    else if (strcmp(m_url, "/register") == 0)
        strcpy(m_url, "/register.html");
    else if (strcmp(m_url, "/picture") == 0)
        strcpy(m_url, "/picture.html");
    else if (strcmp(m_url, "/video") == 0)
        strcpy(m_url, "/video.html");

    // Handle CGI requests (login/registration) based on POST method and URL pattern
    // Assumes URLs like /2... for login and /3... for registration trigger CGI
    // Check if p is not NULL before dereferencing p+1
    if (cgi == 1 && p && p[1] != '\0' && (p[1] == '2' || p[1] == '3'))
    {
        // Extract username and password from m_string (POST body: "user=...")
        char name[100], password[100];
        int name_len = 0;
        int pw_len = 0;
        // Simplified parsing, assuming "user=...&passwd=..." format
        char* user_start = strstr(m_string, "user=");
        if (user_start) {
            user_start += 5; // Move past "user="
            char* user_end = strchr(user_start, '&');
            if (user_end) {
                name_len = user_end - user_start;
                if (name_len >= 100) name_len = 99; // Prevent overflow
                strncpy(name, user_start, name_len);
                name[name_len] = '\0';

                char* pw_start = strstr(user_end, "passwd=");
                if (pw_start) {
                    pw_start += 7; // Move past "passwd="
                    // Find end of password (end of string)
                    char* pw_end = pw_start + strlen(pw_start);
                    pw_len = pw_end - pw_start;
                    if (pw_len >= 100) pw_len = 99; // Prevent overflow
                    strncpy(password, pw_start, pw_len);
                    password[pw_len] = '\0';
                } else { password[0] = '\0'; } // Password not found
            } else { name[0] = '\0'; password[0] = '\0'; } // Malformed body
        } else { name[0] = '\0'; password[0] = '\0'; } // Malformed body


        // Registration (/3...)
        if (p[1] == '3')
        {
            // NOTE: This SQL construction is vulnerable to SQL Injection.
            // Use prepared statements for production code.
            char sql_insert[256];
            snprintf(sql_insert, sizeof(sql_insert), "INSERT INTO user(username, passwd) VALUES('%s', '%s')", name, password);

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                // Assuming 'mysql' is a member variable representing the connection
                int res = mysql_query(mysql, sql_insert);
                if (!res) // SQL query succeeded
                {
                    users.insert(std::make_pair(name, password));
                    m_lock.unlock();
                    // Redirect to login page after successful registration
                    strcpy(m_url, "/login.html");
                }
                else // SQL query failed
                {
                    m_lock.unlock();
                    LOG_ERROR("INSERT error: %s", mysql_error(mysql));
                    // Redirect to registration error page (assuming registerError.html exists)
                    // If not, maybe redirect back to register.html or a generic error page
                    strcpy(m_url, "/register.html"); // Or "/error.html" or "/registerError.html"
                }
            }
            else // User already exists
            {
                 // Redirect to registration error page or back to registration
                 strcpy(m_url, "/register.html"); // Or "/error.html" or "/registerError.html"
            }
        }
        // Login (/2...)
        else // if (p[1] == '2')
        {
            if (users.count(name) && users[name] == password)
            {
                // Login successful, redirect to welcome page
                strcpy(m_url, "/welcome.html");
            }
            else
            {
                // Login failed, redirect back to login page
                strcpy(m_url, "/login.html"); // Or "/loginError.html" if it exists
            }
        }
        // After CGI logic, m_url might have changed (e.g., to /welcome.html).
        // Construct the final file path based on the potentially updated m_url.
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
        m_real_file[FILENAME_LEN - 1] = '\0'; // Ensure null termination
    }
    // General file request (GET, or POST not matching CGI pattern /2 or /3)
    else
    {
        // Append the requested URL path to the document root path
        // Example: doc_root="/path/to/root", m_url="/css/style.css" -> m_real_file="/path/to/root/css/style.css"
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
        m_real_file[FILENAME_LEN - 1] = '\0'; // Ensure null termination
    }

    // --- File checking and mapping logic remains the same ---
    if (stat(m_real_file, &m_file_stat) < 0)
    {
        LOG_ERROR("File not found or inaccessible: %s", m_real_file);
        // Check if the original request was for a specific error page that doesn't exist
        // Avoid infinite loops if error pages themselves are missing
        if (strcmp(m_url, "/404.html") != 0) { // Prevent loop if 404 itself is missing
            strcpy(m_url, "/404.html"); // Try to serve the 404 page
            // Reconstruct path for 404.html
            strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
            m_real_file[FILENAME_LEN - 1] = '\0';
            // Stat again for 404.html
            if (stat(m_real_file, &m_file_stat) < 0) {
                 return NO_RESOURCE; // 404 page also missing, return code
            }
            // Fall through to check permissions etc. for 404.html
        } else {
            return NO_RESOURCE; // Already tried 404, give up
        }
        // If we found 404.html, continue to permission checks etc. below
    }


    if (!(m_file_stat.st_mode & S_IROTH)) // Check if file has 'other' read permission
    {
        LOG_ERROR("Forbidden access to file: %s", m_real_file);
         // Similar logic for 403
        if (strcmp(m_url, "/403.html") != 0) {
            strcpy(m_url, "/403.html");
            strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
            m_real_file[FILENAME_LEN - 1] = '\0';
            if (stat(m_real_file, &m_file_stat) < 0) {
                 return FORBIDDEN_REQUEST; // 403 page missing
            }
             // Fall through if 403.html exists
        } else {
             return FORBIDDEN_REQUEST; // Already tried 403
        }
    }

    if (S_ISDIR(m_file_stat.st_mode)) // Check if it's a directory
    {
        LOG_ERROR("Directory listing forbidden: %s", m_real_file);
        // Similar logic for 400 or 403
        if (strcmp(m_url, "/400.html") != 0) { // Use 400 for bad request (directory)
            strcpy(m_url, "/400.html");
            strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
            m_real_file[FILENAME_LEN - 1] = '\0';
            if (stat(m_real_file, &m_file_stat) < 0) {
                 return BAD_REQUEST; // 400 page missing
            }
             // Fall through if 400.html exists
        } else {
             return BAD_REQUEST; // Already tried 400
        }
    }

    // Open and memory-map the file
    int fd = open(m_real_file, O_RDONLY);
    if (fd < 0) {
        LOG_ERROR("Cannot open file: %s, errno: %d", m_real_file, errno);
        // Don't try to serve 500.html here, just return the error code
        return INTERNAL_ERROR; // 500 Internal Server Error
    }

    // Memory map the file content
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); // Close the file descriptor after mmap

    if (m_file_address == MAP_FAILED) {
        m_file_address = 0; // Reset pointer
        LOG_ERROR("Mmap failed for file: %s, errno: %d", m_real_file, errno);
        // Don't try to serve 500.html here
        return INTERNAL_ERROR; // 500 Internal Server Error
    }

    // File is ready to be sent
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST: // This code is returned for directory requests or other bad requests
    {
        // Send 400 Bad Request response
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form))
            return false;
        break;
    }
    case NO_RESOURCE: // This code is returned for file not found
    {
        // Send 404 Not Found response
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST: // This includes successfully finding regular files AND error pages like 404.html
    {
        // The current design serves error pages (like 404.html) with 200 OK if found by do_request.
        // We maintain that behavior here.
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            // Serve empty body for 0-byte files, but still 200 OK
            const char *ok_string = ""; // Empty body is fine for 0-byte file
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
        break; // Added break statement
    }
    default:
        // Handle unexpected codes by sending an internal server error
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        // Fall through to set up m_iv for sending the error response
    }
    // Common setup for non-FILE_REQUEST cases (or FILE_REQUEST with empty body)
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
