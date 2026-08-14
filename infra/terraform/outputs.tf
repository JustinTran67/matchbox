output "cluster_name" {
  description = "EKS cluster name."
  value       = module.eks.cluster_name
}

output "region" {
  description = "Region the cluster was created in."
  value       = var.region
}

output "cluster_endpoint" {
  description = "EKS API server endpoint."
  value       = module.eks.cluster_endpoint
}

output "update_kubeconfig_command" {
  description = "Run this to point kubectl at the cluster."
  value       = "aws eks update-kubeconfig --region ${var.region} --name ${module.eks.cluster_name}"
}

output "node_group_summary" {
  description = "What the cluster is actually paying for, for the cost write-up."
  value       = "${var.node_count} x ${var.node_instance_type} on-demand, single NAT gateway, ${length(local.azs)} AZs"
}
