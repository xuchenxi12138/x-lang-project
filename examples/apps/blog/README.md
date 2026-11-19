# blog —— Spring Boot 风格的 X 语言应用

一个能跑的最小 Web 应用，演示「分层结构 + 应用框架」怎么在 X 语言里落地。

## 目录结构

```
apps/blog/
├── conf/
│   └── application.properties      # 配置文件（properties 格式）
└── src/
    ├── main/main.xle               # 入口：装配 + 事件循环
    ├── config/app_config.xle       # 类型化配置门脸
    ├── controller/post_controller.xle  # HTTP 接口层
    ├── service/post_service.xle    # 业务逻辑层
    ├── repository/post_repo.xle     # 数据访问层（内存实现）
    ├── model/post.xle              # 实体
    └── middleware/pipeline.xle     # 中间件链
```

对应 Spring 的概念：

| Spring | 本项目 | 说明 |
|---|---|---|
| `@SpringBootApplication` + `main` | `src/main/main.xle` | 装配 + 事件循环 |
| `@ConfigurationProperties` | `src/config/app_config.xle` | 类型化配置 |
| `@RestController` | `src/controller/*.xle` | 接口层 |
| `@Service` | `src/service/*.xle` | 业务层 |
| `@Repository` | `src/repository/*.xle` | 数据访问层 |
| `@Entity` | `src/model/*.xle` | 实体 |
| `Filter` / `HandlerInterceptor` | `src/middleware/pipeline.xle` | 中间件 |
| `@Autowired` | `Container.register` + `attach` | 显式注册（无反射） |
| `application.properties` | `conf/application.properties` | 配置 |
| `/actuator/health` | `/health`、`/health/live`、`/health/ready` | 健康检查 |
| 内嵌 Tomcat | `stdlib/net/http/server.xle` | HTTP 服务 |
| IoC 容器 | `stdlib/web/core/container.xle` | 按名查找 + 单例管理 |

## 运行

```sh
# 在项目根目录
./build/x.exe run apps/blog/src/main/main.xle
```

启动后会打印配置、容器、中间件、路由表，然后监听 8080。

## 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 首页 |
| GET | `/api/posts` | 列表（`?offset=0&limit=20&status=draft&tag=HTTP`） |
| GET | `/api/posts/:id` | 单篇 |
| POST | `/api/posts` | 创建（JSON body） |
| PUT | `/api/posts/:id` | 更新（完整替换） |
| DELETE | `/api/posts/:id` | 删除（204） |
| GET | `/api/posts/status/:s` | 按状态过滤 |
| GET | `/api/posts/tag/:t` | 按标签过滤 |
| GET | `/api/audit` | 审计日志 |
| GET | `/api/health` | 业务健康 |
| GET | `/health/live` | 存活探针（极轻） |
| GET | `/health/ready` | 就绪探针（查依赖） |

### 请求示例

```sh
# 列表
curl 'http://127.0.0.1:8080/api/posts?limit=2'

# 创建
curl -X POST http://127.0.0.1:8080/api/posts \
  -H 'Content-Type: application/json' \
  -d '{"title":"新文章","body":"正文","author":"我","tags":["x"],"status":"published"}'

# 更新
curl -X PUT http://127.0.0.1:8080/api/posts/4 \
  -H 'Content-Type: application/json' \
  -d '{"title":"改过","body":"改过正文","author":"我","status":"published"}'

# 删除
curl -X DELETE http://127.0.0.1:8080/api/posts/4

# 健康
curl http://127.0.0.1:8080/health/ready
```

## 这个示例刻意做对的几件事

### 1. 路由注册顺序 = 匹配优先级

```
/api/posts/status/:s   ← 必须在下面之前
/api/posts/:id
```

反过来的话，`/api/posts/status` 会被 `:id` 匹配走，`id="status"` 变成非法整数返回 400，而真正的路由永远命中不了。这是「注册顺序即优先级」的代价，也是它最容易被忘的地方。

### 2. 状态码语义严格

| 场景 | 码 | 为什么不是别的 |
|---|---|---|
| 创建成功 | 201 + `Location` | 200 会让客户端不知道新资源在哪 |
| 删除成功 | 204 | 没有可返回的内容，200 + body 是多余约定 |
| 校验失败 | 422 | 400 表示「请求本身畸形」，422 表示「格式对但语义错」 |
| 格式不对 | 415 | 400 会让客户端以为参数错了，实际是 Content-Type 错了 |
| 方法不对 | 405 + `Allow` | 404 会让「方法写错」看起来像「路由没注册」 |
| 路径不存在 | 404 | — |

### 3. 错误响应也是 JSON

成功和失败的响应体形状一致，客户端只需要一个解析函数。纯文本错误会让真正的错误（404）被掩盖成「parse error」。

### 4. 标题查重做归一化

`Hello` 和 `hello ` 算重复（去空白 + 小写）。但**存储保留原文** —— 否则用户写的「Hello World」会变成「hello world」，看起来像 bug。

### 5. PUT 是完整替换

缺失字段用空值而不是保留原值。这与 PATCH 不同。混用是经典 API 设计错误：客户端只想改标题，结果正文被清空。

### 6. 健康检查分三层

- `live`：只探进程（不查任何外部依赖）—— 查依赖会让数据库抖动导致所有容器被重启
- `ready`：查所有组件 —— 只有全绿才接流量
- `/api/health`：业务指标

### 7. 启动失败会回滚

配置校验失败 / 监听失败时，已经起的 socket 会被关掉。不回滚的话，反复重启会把端口占满。

## 已知边界

| 项 | 状态 | 原因 |
|---|---|---|
| HTTPS | 不支持 | 语言没有 TLS 层 |
| keep-alive | 不支持 | 单线程模型下复用连接比关闭更慢，明确声明 `Connection: close` |
| 并发 | 单线程 | 一个慢请求会阻塞后续；适合内部工具，不适合高并发公网服务 |
| 接线 | 显式 | 没有反射，Bean 与路由必须手写注册 |
| 存储 | 内存 | 示例刻意不依赖外部环境；真实项目换 `post_repo` 实现即可 |
| 时间戳 | 空 | 语言没有日期时间内建，`createdAt` 始终为空串 |
| 命令行参数 | 不支持 | 语言没有读 argv 的内建，端口只能从配置来 |

## 换成数据库存储

只需新增一个实现同一组方法的仓储，然后改 `main.xle` 里的一行：

```x
// 原来
let repo = new PostRepository()

// 换成
let repo = new PostRepositoryMysql(cfg.mysqlDsn)
```

`PostService` 与 `PostController` 一行都不用改 —— 这就是分层的价值所在。数据库驱动见 `stdlib/db/`（MySQL / PostgreSQL / Redis / MongoDB / Elasticsearch / SQLite / Oracle 七个）。