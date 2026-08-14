#!/usr/bin/env bash
# Deploys matchbox onto the EKS cluster created by infra/terraform, installs the
# observability stack, verifies correctness end to end, and captures evidence.
#
# This script creates NOTHING billable on its own - it assumes `terraform apply` has
# already run. Tear the cluster down with `terraform destroy` when finished; the EKS
# control plane, the nodes and the NAT gateway all bill by the hour until you do.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
TF_DIR="${ROOT}/infra/terraform"
EVIDENCE="${ROOT}/infra/evidence"
LOAD_STEPS="${LOAD_STEPS:-20000}"
LOAD_ROUNDS="${LOAD_ROUNDS:-6}"

mkdir -p "${EVIDENCE}"

echo "==> pointing kubectl at the cluster"
eval "$(terraform -chdir="${TF_DIR}" output -raw update_kubeconfig_command)"

CLUSTER="$(terraform -chdir="${TF_DIR}" output -raw cluster_name)"
REGION="$(terraform -chdir="${TF_DIR}" output -raw region)"
ACCOUNT="$(aws sts get-caller-identity --query Account --output text)"
REGISTRY="${ACCOUNT}.dkr.ecr.${REGION}.amazonaws.com"
IMAGE="${REGISTRY}/matchbox-engine:dev"

echo "==> publishing the image to ECR"
# kind could side-load a local image; a real cluster pulls from a registry, so the image
# has to live somewhere the nodes can reach.
aws ecr describe-repositories --repository-names matchbox-engine --region "${REGION}" >/dev/null 2>&1 \
  || aws ecr create-repository --repository-name matchbox-engine --region "${REGION}" >/dev/null
aws ecr get-login-password --region "${REGION}" | docker login --username AWS --password-stdin "${REGISTRY}"
# EKS nodes are amd64; a laptop build would otherwise produce an arm64 image the nodes
# cannot run, and the pods would crash-loop with an exec format error.
docker buildx build --platform linux/amd64 -t "${IMAGE}" -f "${ROOT}/service/Dockerfile" "${ROOT}" --push

echo "==> checking for a default StorageClass"
# kind ships a default StorageClass; a bare EKS cluster may not, and the Kafka PVC asks for
# the default by name-less reference. Rather than hard-code a class into kafka.yaml (which
# would then break on kind), a default is supplied here only if the cluster lacks one.
if ! kubectl get storageclass -o jsonpath='{.items[*].metadata.annotations.storageclass\.kubernetes\.io/is-default-class}' | grep -q true; then
  echo "    none found; installing gp3 as default"
  kubectl apply -f "${HERE}/storageclass-gp3.yaml"
else
  echo "    default StorageClass already present"
fi

echo "==> deploying kafka (unmodified Phase 4 manifests)"
kubectl apply -f "${HERE}/kafka.yaml"
kubectl wait --for=condition=ready pod/kafka-0 --timeout=600s

echo "==> creating topics"
kubectl apply -f "${HERE}/kafka-topics-job.yaml"
kubectl wait --for=condition=complete job/kafka-topics --timeout=600s

echo "==> deploying the engine StatefulSet"
# The image reference is the only thing that differs from the kind deployment.
kubectl apply -f "${HERE}/engine.yaml"
kubectl set image statefulset/matchbox-engine "engine=${IMAGE}"
kubectl rollout status statefulset/matchbox-engine --timeout=600s

echo "==> installing kube-prometheus-stack"
helm repo add prometheus-community https://prometheus-community.github.io/helm-charts >/dev/null 2>&1 || true
helm repo update >/dev/null
helm upgrade --install monitoring prometheus-community/kube-prometheus-stack \
  --namespace monitoring --create-namespace \
  --set alertmanager.enabled=false \
  --set prometheus.prometheusSpec.replicas=1 \
  --set prometheus.prometheusSpec.retention=6h \
  --set prometheus.prometheusSpec.resources.requests.cpu=200m \
  --set prometheus.prometheusSpec.resources.requests.memory=768Mi \
  --set grafana.replicas=1 \
  --set grafana.sidecar.dashboards.searchNamespace=ALL \
  --set prometheus.prometheusSpec.podMonitorSelectorNilUsesHelmValues=false \
  --set prometheus.prometheusSpec.serviceMonitorSelectorNilUsesHelmValues=false \
  --wait --timeout 15m

echo "==> registering the engine scrape target and dashboard"
kubectl apply -f "${HERE}/engine-servicemonitor.yaml"
kubectl apply -f "${HERE}/grafana-dashboard.yaml"

echo "==> end-to-end correctness check against the real cluster"
kubectl delete pod matchbox-e2e --ignore-not-found >/dev/null
kubectl run matchbox-e2e --image="${IMAGE}" --restart=Never \
  --env="KAFKA_BROKERS=kafka-0.kafka.default.svc.cluster.local:9092" \
  --env="STEPS=5000" --env="WAIT_SECONDS=180" \
  --command -- /usr/local/bin/matchbox_e2e >/dev/null
kubectl wait --for=jsonpath='{.status.phase}'=Succeeded pod/matchbox-e2e --timeout=900s \
  || kubectl wait --for=jsonpath='{.status.phase}'=Failed pod/matchbox-e2e --timeout=10s
kubectl logs matchbox-e2e | tee "${EVIDENCE}/e2e-eks.txt"
E2E_EXIT="$(kubectl get pod matchbox-e2e -o jsonpath='{.status.containerStatuses[0].state.terminated.exitCode}')"

echo "==> generating a sustained load window (${LOAD_ROUNDS} rounds x ${LOAD_STEPS} steps)"
# A single burst would leave the dashboard showing one spike; Prometheus scrapes every 5s,
# so the load has to span minutes for rate() to produce a real curve.
for round in $(seq 1 "${LOAD_ROUNDS}"); do
  kubectl delete pod "matchbox-load-${round}" --ignore-not-found >/dev/null
  kubectl run "matchbox-load-${round}" --image="${IMAGE}" --restart=Never \
    --env="KAFKA_BROKERS=kafka-0.kafka.default.svc.cluster.local:9092" \
    --env="STEPS=${LOAD_STEPS}" --env="WAIT_SECONDS=120" \
    --env="E2E_GROUP=matchbox-load-${round}" \
    --command -- /usr/local/bin/matchbox_e2e >/dev/null
  kubectl wait --for=jsonpath='{.status.phase}'=Succeeded "pod/matchbox-load-${round}" --timeout=600s || true
  echo "    round ${round} done"
done

echo "==> capturing evidence into ${EVIDENCE}"
kubectl get nodes -o wide | tee "${EVIDENCE}/nodes.txt"
kubectl get pods -o wide | tee "${EVIDENCE}/pods.txt"
kubectl get pods -n monitoring -o wide | tee "${EVIDENCE}/pods-monitoring.txt"
kubectl exec matchbox-engine-0 -- /bin/sh -c 'exit 0' 2>/dev/null || true
kubectl run metrics-probe --rm -i --restart=Never --image=curlimages/curl:latest -- \
  -s http://matchbox-engine-0.matchbox-engine.default.svc.cluster.local:8080/metrics \
  | tee "${EVIDENCE}/metrics-engine-0.txt"

echo "==> querying prometheus for the same panels the dashboard renders"
kubectl run promquery --rm -i --restart=Never --image=curlimages/curl:latest -- \
  -s --get --data-urlencode 'query=histogram_quantile(0.99, sum(rate(matchbox_submit_latency_seconds_bucket[5m])) by (le))' \
  http://monitoring-kube-prometheus-prometheus.monitoring.svc.cluster.local:9090/api/v1/query \
  | tee "${EVIDENCE}/prometheus-p99.json"

echo
echo "==> e2e exit code: ${E2E_EXIT}"
echo "==> Grafana user is admin; read its generated password with:"
echo "    kubectl -n monitoring get secret monitoring-grafana -o jsonpath=\"{.data.admin-password}\" | base64 -d; echo"
echo "==> TEAR DOWN when finished:  terraform -chdir=${TF_DIR} destroy"
exit "${E2E_EXIT}"
