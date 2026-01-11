import http.server
import ssl

# Define the server address and port
server_address = ('localhost', 3000)

# Create the HTTP Server
httpd = http.server.HTTPServer(server_address, http.server.SimpleHTTPRequestHandler)

# Create an SSL Context
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(certfile='./cert.pem', keyfile='./key.pem')

# Wrap the socket with SSL
httpd.socket = context.wrap_socket(httpd.socket, server_side=True)

print("Serving HTTPS on https://localhost:3000")
httpd.serve_forever()
