#include "TlsClient.hpp"

#include "projdefs.h"
#include <lwip/altcp.h>
#include <lwip/dns.h>
#include <mbedtls/ssl.h>
#include <pico/cyw43_arch.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Network
{

Client::Client(uint8_t timeout, QueueHandle_t targetQueue, const uint8_t *cert, size_t cert_len) :
    m_Timeout{timeout},
    m_TargetQueue{targetQueue}
{
    m_Config = altcp_tls_create_config_client(cert, cert_len);
    assert(m_Config);

    mbedtls_ssl_conf_authmode((mbedtls_ssl_config *)m_Config,
                              MBEDTLS_SSL_VERIFY_OPTIONAL);
}

Client::~Client() { altcp_tls_free_config(m_Config); }

int Client::send(const std::string &request)
{
    m_Request = request;
    setResponseStatus(0);
    open();

    while (m_ResponseStatus == 0) { vTaskDelay(pdMS_TO_TICKS(1)); }

    return m_ResponseStatus;
}

int Client::getError() const { return m_Error; }

void Client::setError(int Error) { m_Error = Error; }

std::string Client::getRequest() const { return m_Request; }

int Client::getStatus(const char *buffer, uint16_t count)
{
    int status = -1;
    const char *substr = strstr(buffer, "Status: ");

    if (substr != NULL) { sscanf(substr, "Status: %d", &status); }
    return status;
}

void Client::setResponseStatus(int status) { m_ResponseStatus = status; }

void Client::getTarget(const char *buffer, uint16_t count)
{
    uint32_t value;
    const char *substr = strstr(buffer, "target=");

    if (substr != NULL && sscanf(substr, "target=%u", &value) == 1)
    {
        xQueueOverwrite(m_TargetQueue, &value);
    }
}

altcp_pcb *Client::getPcb() const { return m_Pcb; }

bool Client::open()
{
    err_t err;
    ip_addr_t server_ip;

    m_Pcb = altcp_tls_new(m_Config, IPADDR_TYPE_ANY);

    if (!m_Pcb)
    {
        printf("failed to create pcb\n");
        return false;
    }

    altcp_arg(m_Pcb, this);
    altcp_poll(m_Pcb, tlsPoll, m_Timeout * 2);
    altcp_recv(m_Pcb, tlsReceive);
    altcp_err(m_Pcb, tlsError);

    /* Set SNI */
    mbedtls_ssl_set_hostname(static_cast<mbedtls_ssl_context *>(altcp_tls_context(m_Pcb)),
                             m_Hostname.c_str());

    printf("resolving %s\n", m_Hostname.c_str());

    // cyw43_arch_lwip_begin/end should be used around calls into lwIP to ensure correct locking.
    // You can omit them if you are in a callback from lwIP. Note that when using pico_cyw_arch_poll
    // these calls are a no-op and can be omitted, but it is a good practice to use them in
    // case you switch the cyw43_arch type later.
    cyw43_arch_lwip_begin();

    err = dns_gethostbyname(m_Hostname.c_str(), &server_ip, tlsDnsFound, this);

    if (err == ERR_OK)
    {
        /* host is in DNS cache */
        connect(&server_ip);
    }
    else if (err != ERR_INPROGRESS)
    {
        printf("error initiating DNS resolving, err=%d\n", err);
        close();
    }

    cyw43_arch_lwip_end();

    return err == ERR_OK || err == ERR_INPROGRESS;
}

void Client::connect(const ip_addr_t *ipaddr)
{
    err_t err;
    u16_t port = 443;

    printf("connecting to server IP %s port %d\n", ipaddr_ntoa(ipaddr), port);
    err = altcp_connect(m_Pcb, ipaddr, port, tlsConnected);
    if (err != ERR_OK)
    {
        fprintf(stderr, "error initiating connect, err=%d\n", err);
        close();
    }
}

err_t Client::close()
{
    err_t err = ERR_OK;
    m_Complete = true;

    if (m_Pcb != NULL)
    {
        altcp_arg(m_Pcb, NULL);
        altcp_poll(m_Pcb, NULL, 0);
        altcp_recv(m_Pcb, NULL);
        altcp_err(m_Pcb, NULL);
        err = altcp_close(m_Pcb);

        if (err != ERR_OK)
        {
            printf("close failed %d, calling abort\n", err);
            altcp_abort(m_Pcb);
            err = ERR_ABRT;
        }
        m_Pcb = NULL;
    }
    return err;
}

err_t Client::tlsConnected(void *arg, struct altcp_pcb *pcb, err_t err)
{
    Client *client = static_cast<Client *>(arg);

    if (err != ERR_OK)
    {
        printf("connect failed %d\n", err);
        return client->close();
    }

    printf("connected to server, sending request\n");

    err = altcp_write(client->getPcb(),
                      (client->getRequest()).c_str(),
                      (client->getRequest()).size(),
                      TCP_WRITE_FLAG_COPY);

    if (err != ERR_OK)
    {
        printf("error writing data, err=%d", err);
        return client->close();
    }

    return ERR_OK;
}

err_t Client::tlsReceive(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err)
{
    Client *client = static_cast<Client *>(arg);

    if (!p)
    {
        printf("connection closed\n");
        return client->close();
    }

    if (p->tot_len > 0)
    {
        /* For simplicity this examples creates a buffer on stack the size of the data pending here,
           and copies all the data to it in one go.
           Do be aware that the amount of data can potentially be a bit large (TLS record size can be 16 KB),
           so you may want to use a smaller fixed size buffer and copy the data to it using a loop, if memory is a concern */
        // char buf[p->tot_len + 1];
        char *buf = (char *)malloc(p->tot_len + 1);

        pbuf_copy_partial(p, buf, p->tot_len, 0);
        buf[p->tot_len] = 0;

        // printf("***\nnew data received from server:\n***\n\n%s\n", buf);
        int status = client->getStatus(buf, p->tot_len);
        if (status == 200) { client->getTarget(buf, p->tot_len); }
        else if (status != -1)
        {
            printf("Error sending Thingspeak data. Check API key.\n");
        }
        client->setResponseStatus(status);

        free(buf);

        altcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);

    return ERR_OK;
}

void Client::tlsDnsFound(const char *hostname, const ip_addr_t *ipaddr, void *arg)
{
    Client *client = static_cast<Client *>(arg);

    if (ipaddr)
    {
        printf("DNS resolving complete\n");
        client->connect(ipaddr);
    }
    else
    {
        printf("error resolving hostname %s\n", hostname);
        client->close();
    }
}

err_t Client::tlsPoll(void *arg, struct altcp_pcb *pcb)
{
    Client *client = static_cast<Client *>(arg);

    printf("timed out\n");
    client->setError(PICO_ERROR_TIMEOUT);
    return client->close();
}

void Client::tlsError(void *arg, err_t err)
{
    Client *client = static_cast<Client *>(arg);

    printf("tls_client_err %d\n", err);
    client->close();
    client->setError(PICO_ERROR_GENERIC);
}

} // namespace Network
