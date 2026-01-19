#pragma once

const char* kMonitorHtml = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>RPC 服务监控 (RPC Monitor)</title>
    <script src="/js/jquery_min"></script>
    <script src="/js/flot_min"></script>
    <style>
        body { margin: 0; padding: 0; font-family: "Helvetica Neue", Helvetica, Arial, sans-serif; font-size: 14px; color: #333; background-color: #f8f9fa; }
        
        /* 导航栏 */
        .navbar { background-color: #343a40; color: #fff; padding: 0 20px; height: 50px; display: flex; align-items: center; justify-content: space-between; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
        .navbar-brand { font-size: 18px; font-weight: bold; color: #fff; text-decoration: none; margin-right: 30px; }
        .navbar-nav { list-style: none; margin: 0; padding: 0; display: flex; }
        .navbar-nav li { margin-right: 20px; }
        .navbar-nav li a { color: #ced4da; text-decoration: none; padding: 15px 0; border-bottom: 2px solid transparent; transition: all 0.2s; display: block; line-height: 20px; }
        .navbar-nav li a:hover { color: #fff; }
        .navbar-nav li.active a { color: #fff; border-bottom: 2px solid #fff; }
        
        /* 内容区域 */
        .content-wrapper { padding: 20px; max-width: 1200px; margin: 0 auto; }
        .tab-pane { display: none; }
        .tab-pane.active { display: block; }
        
        /* 卡片容器 */
        .card { background: #fff; border-radius: 4px; box-shadow: 0 1px 3px rgba(0,0,0,0.1); margin-bottom: 20px; padding: 20px; }
        .card-header { font-size: 16px; font-weight: bold; border-bottom: 1px solid #eee; padding-bottom: 10px; margin-bottom: 15px; color: #495057; }
        
        /* 变量样式 (类似 BRPC) */
        .variable-container { border-bottom: 1px solid #f1f1f1; }
        .variable { 
            padding: 8px 10px; 
            cursor: pointer; 
            color: #212529; 
            display: flex;
            justify-content: space-between;
            align-items: center;
            background-color: #fff; 
            transition: background-color 0.1s;
        }
        .variable:hover { background-color: #e9ecef; }
        .variable .var-name { font-weight: 500; color: #007bff; }
        .variable .var-labels { color: #868e96; font-size: 0.85em; margin-left: 10px; }
        .variable .var-value { font-family: SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace; font-weight: bold; color: #28a745; }
        
        .detail { margin: 0; padding: 15px; background-color: #fafafa; border-top: 1px solid #f1f1f1; display: none; }
        .flot-placeholder { width: 100%; height: 250px; font-size: 14px; line-height: 1.2em; }
        
        /* 状态表格 */
        table.status-table { width: 100%; border-collapse: collapse; }
        table.status-table th, table.status-table td { text-align: left; padding: 10px; border-bottom: 1px solid #eee; }
        table.status-table th { width: 200px; color: #6c757d; font-weight: 500; }
        table.status-table td { font-weight: 500; }

        /* 服务列表 */
        .service-item { margin-bottom: 15px; }
        .service-name { font-size: 16px; font-weight: bold; color: #333; margin-bottom: 5px; }
        .method-list { list-style: none; padding-left: 20px; margin: 0; }
        .method-list li { color: #666; padding: 2px 0; }
        
        /* 搜索框 */
        .search-container { margin-bottom: 15px; }
        #searchbox { padding: 8px 12px; width: 100%; box-sizing: border-box; border: 1px solid #ced4da; border-radius: 4px; font-size: 14px; }
        
        /* 提示框 */
        #tooltip { position: absolute; display: none; border: 1px solid #ccc; padding: 4px 8px; background-color: rgba(255, 255, 255, 0.9); font-size: 12px; border-radius: 3px; z-index: 1000; box-shadow: 0 2px 4px rgba(0,0,0,0.1); pointer-events: none; }
    </style>
</head>
<body>

    <div class="navbar">
        <a href="#" class="navbar-brand">RPC 服务监控</a>
        <ul class="navbar-nav">
            <li class="active"><a href="#" onclick="switchTab('vars', this); return false;">监控指标 (Vars)</a></li>
            <li><a href="#" onclick="switchTab('services', this); return false;">服务列表 (Services)</a></li>
            <li><a href="#" onclick="switchTab('status', this); return false;">系统状态 (Status)</a></li>
        </ul>
    </div>

    <div class="content-wrapper">
        <!-- Vars Tab -->
        <div id="tab-vars" class="tab-pane active">
            <div class="card">
                <div class="search-container">
                    <input id="searchbox" type="text" placeholder="搜索指标 (例如 latency, qps)..." onkeyup="filterVars()">
                </div>
                <div id="vars-list">正在加载指标...</div>
            </div>
        </div>

        <!-- Services Tab -->
        <div id="tab-services" class="tab-pane">
            <div class="card">
                <div class="card-header">已注册服务 (Registered Services)</div>
                <div id="services-list">正在加载服务列表...</div>
            </div>
        </div>

        <!-- Status Tab -->
        <div id="tab-status" class="tab-pane">
            <div class="card">
                <div class="card-header">服务器状态 (Server Status)</div>
                <div id="server-status">正在加载状态...</div>
            </div>
        </div>
    </div>

    <script>
        // --- 状态管理 ---
        var enabled = {};     // 当前展开的图表
        var timeouts = {};    // 图表轮询的定时器 ID
        var allMetrics = [];  // 所有指标的缓存
        var lastPlot = {};    // Flot 图表对象

        // --- 标签页切换 ---
        function switchTab(tabName, linkEl) {
            $(".tab-pane").removeClass("active");
            $(".navbar-nav li").removeClass("active");
            
            $("#tab-" + tabName).addClass("active");
            if(linkEl) $(linkEl).parent().addClass("active");

            if (tabName === 'status' || tabName === 'services') fetchStatus();
        }

        // --- 指标列表逻辑 ---
        function fetchVarsList() {
            $.getJSON("/metrics_json", function(data) {
                allMetrics = data.metrics;
                renderVarsList();
            }).fail(function(jqXHR, textStatus, errorThrown) {
                console.error("Failed to fetch vars: " + textStatus);
                $("#vars-list").text("加载指标失败: " + textStatus);
            });
        }

        function renderVarsList() {
            var container = $("#vars-list");
            var filter = $("#searchbox").val().toLowerCase();
            
            if (container.text() === "正在加载指标...") container.empty();
            
            // 查找连接数指标
            var connMetric = allMetrics.find(function(m) { return m.name === "rpc_connection_count"; });
            if (connMetric) {
                var connId = "conn-count-display";
                if ($("#" + connId).length === 0) {
                    var connHtml = '<div id="' + connId + '" class="alert alert-info" style="margin-bottom: 20px; padding: 15px;">' +
                                   '<h4 style="margin:0">当前连接数 (Active Connections): <span id="val-' + connId + '" style="font-weight:bold">' + connMetric.value + '</span></h4>' +
                                   '</div>';
                    container.prepend(connHtml);
                } else {
                    $("#val-" + connId).text(connMetric.value);
                }
            }

            allMetrics.forEach(function(m) {
                if (m.name === "rpc_connection_count") return; // 跳过，已在顶部显示

                var id = m.id;
                var elId = "var-" + id.replace(/[^a-zA-Z0-9]/g, "_");
                var nameStr = m.name;
                var labelStr = "";
                for (var k in m.labels) {
                    labelStr += k + "=" + m.labels[k] + " ";
                }
                
                // 过滤逻辑
                var fullText = (nameStr + " " + labelStr).toLowerCase();
                if (filter && fullText.indexOf(filter) === -1) {
                    $("#wrapper-" + elId).hide();
                    return;
                }
                
                // 数值格式化
                var val = "";
                if (m.type === "histogram") {
                    val = "QPS: " + m.qps + " | Max: " + m.max_latency.toFixed(4) + "s";
                } else {
                    val = m.value;
                }

                if ($("#" + elId).length === 0) {
                    // 新增指标
                    var html = '<div id="wrapper-' + elId + '" class="variable-container">' +
                               '<div class="variable" id="' + elId + '" data-id="' + id + '" data-type="' + m.type + '">' + 
                               '<div><span class="var-name">' + nameStr + '</span>' +
                               '<span class="var-labels">' + (labelStr ? "{" + labelStr + "}" : "") + '</span></div>' +
                               '<span class="var-value" id="val-' + elId + '">' + val + '</span>' +
                               '</div>' +
                               '<div class="detail" id="detail-' + elId + '">' +
                               (m.type === "histogram" ? '<div class="flot-placeholder" id="chart-' + elId + '"></div>' : '<div style="color:#999; padding:10px;">该类型无历史数据</div>') +
                               '</div></div>';
                    container.append(html);
                    
                    // 点击事件处理
                    $("#" + elId).click(function() {
                        var detail = $("#detail-" + elId);
                        detail.slideToggle("fast");
                        if (m.type === "histogram") {
                            toggleGraph(id, elId);
                        }
                    });
                } else {
                    // 更新现有指标
                    $("#wrapper-" + elId).show();
                    $("#val-" + elId).text(val);
                }
            });
        }

        function filterVars() {
            renderVarsList();
        }

        // --- 图表逻辑 ---
        var trendOptions = {
            colors: ['#007bff', '#dc3545', '#ffc107'], // QPS(蓝), 最大延迟(红), 平均延迟(黄)
            legend: { position: "nw", backgroundOpacity: 0.8 },
            grid: { hoverable: true, borderColor: "#eee", borderWidth: 1 },
            xaxis: { 
                tickFormatter: function(val, axis) {
                    var d = new Date(val);
                    return d.getHours().toString().padStart(2,'0') + ":" + 
                           d.getMinutes().toString().padStart(2,'0') + ":" + 
                           d.getSeconds().toString().padStart(2,'0');
                }
            },
            yaxes: [{ min: 0 }, { position: "right", min: 0, tickFormatter: function(v) { return v.toFixed(4) + "s"; } }],
            lines: { show: true, lineWidth: 2, fill: true, fillColor: { colors: [{ opacity: 0.1 }, { opacity: 0.3 }] } },
            points: { show: false }
        };

        function toggleGraph(metricId, elId) {
            if (enabled[metricId]) {
                enabled[metricId] = false;
                clearTimeout(timeouts[metricId]);
            } else {
                enabled[metricId] = true;
                fetchData(metricId, elId);
            }
        }

        function fetchData(metricId, elId) {
            if (!enabled[metricId]) return;
            
            $.getJSON("/metrics_series?name=" + encodeURIComponent(metricId), function(series) {
                 lastPlot[metricId] = $.plot("#chart-" + elId, series, trendOptions);
            }).fail(function() {
                 console.log("Failed to fetch series for " + metricId);
            });
            
            timeouts[metricId] = setTimeout(function() { fetchData(metricId, elId); }, 1000);
        }

        // --- 状态页逻辑 ---
        function fetchStatus() {
            $.getJSON("/status", function(data) {
                // 渲染状态页
                var statusHtml = "<table class='status-table'>";
                statusHtml += "<tr><th>状态 (Status)</th><td>" + (data.status === "ok" ? "<span style='color:green'>正常 (OK)</span>" : "<span style='color:red'>错误 (Error)</span>") + "</td></tr>";
                statusHtml += "<tr><th>版本 (Version)</th><td>" + (data.version || "-") + "</td></tr>";
                statusHtml += "<tr><th>当前连接数 (Active Connections)</th><td>" + (data.connection_count || 0) + "</td></tr>";
                statusHtml += "<tr><th>最大并发 (Max Concurrency)</th><td>" + (data.max_concurrency || "无限制") + "</td></tr>";
                statusHtml += "<tr><th>启动时间 (Start Time)</th><td>" + new Date(data.start_time * 1000).toLocaleString() + "</td></tr>";
                statusHtml += "</table>";
                $("#server-status").html(statusHtml);

                // 渲染服务列表页
                var servicesHtml = "";
                if (data.services && data.services.length > 0) {
                    data.services.forEach(function(svc) {
                        servicesHtml += '<div class="service-item">';
                        servicesHtml += '<div class="service-name">' + svc.name + '</div>';
                        servicesHtml += '<ul class="method-list">';
                        svc.methods.forEach(function(m) {
                            servicesHtml += '<li>' + m + '</li>';
                        });
                        servicesHtml += '</ul></div>';
                    });
                } else {
                    servicesHtml = "暂无注册服务。";
                }
                $("#services-list").html(servicesHtml);
            }).fail(function() {
                $("#server-status").html("加载状态失败。");
                $("#services-list").html("加载服务列表失败。");
            });
        }

        // --- 提示框 ---
        $("<div id='tooltip'></div>").appendTo("body");

        $(document).bind("plothover", function (event, pos, item) {
            if (item) {
                var x = new Date(item.datapoint[0]).toLocaleTimeString();
                var y = item.datapoint[1].toFixed(4);
                var label = item.series.label;
                // 翻译标签
                if(label === "QPS") label = "QPS (请求/秒)";
                if(label === "Max Latency") label = "最大延迟";
                if(label === "Avg Latency") label = "平均延迟";

                $("#tooltip").html("<strong>" + label + "</strong><br>" + y + "<br><em>" + x + "</em>")
                    .css({top: item.pageY+5, left: item.pageX+5})
                    .fadeIn(100);
            } else {
                $("#tooltip").hide();
            }
        });

        // --- 初始化 ---
        $(function() {
            fetchVarsList();
            setInterval(fetchVarsList, 2000); // 每2秒轮询一次列表
        });
    </script>
</body>
</html>
)HTML";
